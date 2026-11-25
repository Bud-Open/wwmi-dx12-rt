    // Runs under the existing device Runtime lock. This discovery path never
    // publishes an OcclusionNativeScene or returns draw replacement authority.
    const AuthorPlan *occlusion_probe_plan(const OcclusionProbeTarget &target){
        const AuthorPlan *plan=nullptr;
        for(const auto &choice:author_plans){
            if(choice->disposition!=WWMI_RT_AUTHOR_PRIVATE||!choice->pack||!choice->pose||!choice->identity||
               choice->identity->allocation!=target.native_blas||choice->identity->recording!=target.native_pose||
               choice->identity->native_blas!=target.address||
               std::find(catalog.packs.begin(),catalog.packs.end(),choice->pack)==catalog.packs.end()||
               !inputs.resource_current(choice->primitive))continue;
            if(plan)return nullptr;
            plan=choice.get();
        }
        return plan;
    }
    std::optional<OcclusionProbeTargets> occlusion_probe_targets(){
        // Two native pose rings can both be live. Preserve the finite set
        // of known target geometry, not a guessed latest Primitive.
        OcclusionProbeTargets found;
        for(const auto &h:surface_history){
            const auto &source=h.identity;
            if(h.pack||!source||source->components.empty()||
               inputs.resource_at(source->native_blas)!=source->allocation||
               std::none_of(catalog.packs.begin(),catalog.packs.end(),[&](const auto &p){
                   return p->native==source->components.front().sample.profile;
               })||std::any_of(source->components.begin(),source->components.end(),
               [&](const auto &part){return !identities.live(source,part.sample.component);}))continue;
            const auto begin=found.entries.begin();
            const auto end=begin+found.count;
            if(std::any_of(begin,end,[&](const auto &old){
                return old.native_blas==source->allocation&&old.native_pose==source->recording&&old.address==source->native_blas;
            }))continue;
            if(found.count==found.capacity)return {};
            found.entries[found.count++]={source->allocation,{},source->recording,source->native_blas};
        }
        return found;
    }
    void note_occlusion_probe_plans(OcclusionProbeTargets &targets){
        for(unsigned i=0;i<targets.count;++i){
            auto &target=targets.entries[i];
            if(target.plan)continue;
            if(const auto *plan=occlusion_probe_plan(target)){
                target.primitive=plan->primitive;target.primitive_offset=plan->scope.byte_origin;
                target.plan=plan->id;target.revision=plan->pack->revision;
            }
        }
    }
    void occlusion_probe_status(unsigned bit,const char *stage,const RayBuildUse *build=nullptr,int status=0){
        if(instance_probe_reasons&bit)return;
        instance_probe_reasons|=bit;
        char line[355];
        std::snprintf(line,sizeof(line),"RT-RAY-INSTANCE-INPUT stage=%s authority=none status=%d "
            "watch=%llx/%llu+%llu/%llu build=%llx/%llu/%llu/%llu heap=%u",
            stage,status,(unsigned long long)instance_probe_watch.resource.object,
            (unsigned long long)instance_probe_watch.resource.creation,(unsigned long long)instance_probe_watch.offset,
            (unsigned long long)instance_probe_watch.bytes,(unsigned long long)(build?build->recording.list:0),
            (unsigned long long)(build?build->recording.creation:0),(unsigned long long)(build?build->recording.generation:0),
            (unsigned long long)(build?build->order:0),unsigned(build?build->instances.heap:api::memory_heap::unknown));
        reshade::log::message(reshade::log::level::info,line);
    }
    void note_occlusion_instance_input(api::command_list *list,const RaySceneInput &ray){
        if(!is_occlusion(ray))return;
        const auto epoch=inputs.recording(list);
        const auto build=ray_scene.build_use(inputs,epoch,ray);
        if(!build)return;
        if(instance_probe){
            if(!instance_probe->use.list&&occlusion_build_matches(*build,instance_probe->build)){
                instance_probe->use=epoch;
                note_occlusion_probe_plans(instance_probe->targets);
                settle_occlusion_instance_probe();
            }
        }else if(!instance_probe_watch.resource){
            // Start only after actual p4 typed use identified a native Build.
            // A subsequent original Build must request this very allocation
            // generation and byte window; the later consumer joins it again.
            instance_probe_watch={build->instances.key,build->instances.offset,build->instances.bytes};
            instance_probe_source_address=build->instances_va;
            instance_dispatch_hint=build->recording;
            if(register_forward_v2&&capture_occlusion_compute)
                instance_dispatch_watch.store(instance_dispatch_hint.list,std::memory_order_relaxed);
            occlusion_probe_status(1,"armed", &*build);
        }
    }
    void note_occlusion_native_dispatch(api::command_list *list,const WWMI_RtNativeDispatch_v2 &event){
        if(instance_probe||!instance_probe_watch.resource||!capture_occlusion_compute)return;
        const auto epoch=inputs.recording(list);
        if(!epoch.list||epoch.list!=instance_dispatch_hint.list||epoch.creation!=instance_dispatch_hint.creation)return;
        if(event.phase==WWMI_RT_DISPATCH_NATIVE_RECORDED){
            if(instance_dispatch_pending.call==event.call_id&&instance_dispatch_pending.recording==epoch&&
               instance_dispatch_pending.pipeline==event.pipeline&&instance_dispatch_pending.layout==event.layout&&
               instance_dispatch_pending.x==event.group_x&&instance_dispatch_pending.y==event.group_y&&
               instance_dispatch_pending.z==event.group_z){
                instance_dispatch_recorded=instance_dispatch_pending;
                instance_dispatch_pending={};
            }
            return;
        }
        if(event.phase==WWMI_RT_DISPATCH_CANCELLED){
            if(instance_dispatch_pending.call==event.call_id&&instance_dispatch_pending.recording==epoch)
                instance_dispatch_pending={};
            return;
        }
        if(event.phase!=WWMI_RT_DISPATCH_BEFORE_NATIVE||!(event.flags&WWMI_RT_DISPATCH_STATE_KNOWN)||
           !event.call_id||!event.group_x||!event.group_y||!event.group_z)return;
        instance_dispatch_pending={};
        WWMI_ComputeRestoreInfo_v1 state{sizeof(state)};
        std::array<WWMI_ComputeRestoreRecord_v1,64> records;
        std::uint32_t words[64];
        if(capture_occlusion_compute(event.api_list,&state,records.data(),unsigned(records.size()),words,64)||
           state.version!=1||state.native_list!=event.native_list||state.root_signature!=event.layout||
           state.level!=WWMI_COMPUTE_RESTORE_ROOT_ARGUMENTS||state.record_count>records.size())return;
        const WWMI_ComputeRestoreRecord_v1 *match=nullptr;
        for(unsigned i=0;i<state.record_count;++i){
            const auto &a=records[i];
            if(a.kind!=WWMI_COMPUTE_ROOT_UAV||!a.value||a.value>instance_probe_source_address)continue;
            const auto leading=instance_probe_source_address-a.value;
            if(leading>UINT64_MAX-instance_probe_watch.bytes)continue;
            const auto range=inputs.ray_input_range(a.value,leading+instance_probe_watch.bytes);
            if(range.key!=instance_probe_watch.resource||range.offset>instance_probe_watch.offset||
               instance_probe_watch.offset-range.offset!=leading)continue;
            if(match)return;
            match=&a;
        }
        if(match)instance_dispatch_pending={epoch,event.call_id,event.pipeline,event.layout,match->value,match->parameter,
            event.group_x,event.group_y,event.group_z};
    }
    void note_occlusion_copy(api::command_list *list,api::resource src,std::uint64_t src_offset,
        api::resource dst,std::uint64_t dst_offset,std::uint64_t bytes){
        const auto &window=instance_probe_watch;
        if(instance_probe||dst.handle!=window.resource.object||!window.resource||!ray_input_window()||
           dst_offset>window.offset||window.offset-dst_offset>bytes||window.bytes>bytes-(window.offset-dst_offset)||
           inputs.key_of(dst)!=window.resource)return;
        // Existing SDK notification is before the real copy, possibly
        // cancelled. This is a precise source lead, never a writer seal.
        const auto source_key=inputs.key_of(src);
        if(source_key)instance_probe_copy={inputs.recording(list),source_key,src_offset,dst_offset,bytes};
    }
    void record_occlusion_instance_probe(api::command_list *list,const RayBuildUse &build){
        if(instance_probe||(instance_probe_reasons&16)||!instance_probe_watch.matches(build.instances))return;
        if(!build.count||build.instances.bytes!=64ull*build.count||build.instances.bytes>OcclusionInstanceProbe::max_source_bytes||
           build.instances_va%D3D12_RAYTRACING_INSTANCE_DESCS_BYTE_ALIGNMENT){
            occlusion_probe_status(2,"invalid_or_large_window",&build);return;
        }
        auto targets=occlusion_probe_targets();
        if(!targets){occlusion_probe_status(4,"target_geometry_set_overflow",&build);return;}
        if(!targets->count){occlusion_probe_status(2048,"no_live_native_geometry",&build);return;}
        note_occlusion_probe_plans(*targets);
        WWMI_ComputeStateInfo_v1 state{sizeof(state)};
        const auto key=reinterpret_cast<std::uint64_t>(list);
        if(const auto rc=compute_query(key,&state)){
            occlusion_probe_status(8,"compute_restore_unavailable",&build,rc);return;
        }
        std::shared_ptr<OcclusionInstanceProbe> probe;
        try{
            if(!instance_probe_program)instance_probe_program=std::make_shared<OcclusionProbePrograms>(
                native.Get(),read_asset(L"ProbeRtInstanceRows.cso"));
            probe=std::make_shared<OcclusionInstanceProbe>();
            probe->build=build;probe->program=instance_probe_program;probe->state_at_build=state;
            probe->targets=*targets;
            if(instance_probe_copy.recording==build.recording)probe->copy_request=instance_probe_copy;
            if(instance_dispatch_recorded.recording==build.recording)probe->native_dispatch=instance_dispatch_recorded;
            if(!inputs.resource_current(build.instances.key))return;
            // The original native Build is about to use this allocation.
            // Hold it now, in that synchronous live scope, through replays.
            probe->source=reinterpret_cast<ID3D12Resource *>(build.instances.key.object);
            const auto desc=probe->source->GetDesc();
            if(desc.Dimension!=D3D12_RESOURCE_DIMENSION_BUFFER||(desc.Flags&D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)||
               build.instances.offset>desc.Width||build.instances.bytes>desc.Width-build.instances.offset){
                occlusion_probe_status(2,"native_buffer_not_srv_readable",&build);return;
            }
            probe->output=gpu::buffer(native.Get(),sizeof(OcclusionInstanceProbe::Result),D3D12_HEAP_TYPE_DEFAULT,
                D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            probe->readback=gpu::buffer(native.Get(),sizeof(OcclusionInstanceProbe::Result),D3D12_HEAP_TYPE_READBACK,
                D3D12_RESOURCE_STATE_COPY_DEST);
            // Reuse the HRESULT-aware Reset handshake. A failed Reset must
            // not lose the recording's hold if the game submits it later.
            if(!hold_draw_recording(build.recording,list))return;
            records[build.recording.list].push_back(probe);
        }catch(const std::exception &){
            occlusion_probe_status(16,"probe_asset_or_allocation_failed",&build);return;
        }
        // This is before the actual native TOP_LEVEL ARRAY Build, after
        // cancellation was ruled out. Its InstanceDescs must be legal for
        // NON_PIXEL_SHADER_RESOURCE. No game-resource transition is needed.
        const auto rc=compute_run(key,OcclusionInstanceProbe::work,probe.get());
        if(!probe->recorded){
            records[build.recording.list].pop_back(); // Run did not emit any command
            occlusion_probe_status(8,"compute_restore_unavailable",&build,rc);
            return;
        }
        instance_probe=std::move(probe);
        auto hint=instance_dispatch_hint.list;
        instance_dispatch_watch.compare_exchange_strong(hint,0,std::memory_order_relaxed);
        occlusion_probe_status(32,"gpu_recorded",&build,rc);
    }
    void settle_occlusion_instance_probe(){
        auto *probe=instance_probe.get();
        if(!probe){
            if(instance_probe_watch.resource&&!ray_input_window()){
                auto hint=instance_dispatch_hint.list;
                instance_dispatch_watch.compare_exchange_strong(hint,0,std::memory_order_relaxed);
                occlusion_probe_status(64,"window_ended_without_recording");
            }
            return;
        }
        if(probe->reported)return;
        const auto &build=probe->build;
        if(!probe->use.list){
            if(!ray_input_window()){
                probe->reported=true;
                occlusion_probe_status(128,"recorded_without_matching_mainrg",&build);
            }
            return;
        }
        if(probe->pre>1){
            probe->reported=true;
            occlusion_probe_status(256,"replayed_before_readback",&build);
            return;
        }
        if(!probe->pre&&inputs.recording(reinterpret_cast<api::command_list *>(build.recording.list))!=build.recording){
            probe->reported=true;
            occlusion_probe_status(1024,"recording_retired_without_submit",&build);return;
        }
        if(probe->pre!=1||probe->post!=1||!writer_signal_known(probe->completion,true))return;
        probe->reported=true;
        OcclusionInstanceProbe::Result result{};
        if(!probe->read_completed(result)){
            occlusion_probe_status(512,"readback_map_failed",&build);return;
        }
        const auto order=(std::uint64_t(result.order_high)<<32)|result.order_low;
        const bool covered=order==build.order&&result.covered==build.count&&result.total<=build.count;
        const unsigned stored=covered?(std::min)(result.total,OcclusionInstanceProbe::capacity):0;
        const auto &copy=probe->copy_request;const auto &dispatch=probe->native_dispatch;
        char line[1200];
        std::snprintf(line,sizeof(line),"RT-RAY-INSTANCE-INPUT stage=%s authority=none "
            "use=%llx/%llu/%llu build=%llx/%llu/%llu/%llu instance=%llx/%llu+%llu/%llu heap=%u "
            "rows=%u covered=%u target_geometries=%u candidate_rows=%u stored=%u overflow=%u queue=%llx fence=%llx/%llu "
            "compute_bound_at_build=%llx/%llx/%u copy_request_source=%llx/%llu+%llu dst_offset=%llu copy_bytes=%llu "
            "native_dispatch_call=%llu pso=%llx layout=%llx root_uav=%u/%llx groups=%u,%u,%u",
            covered?"gpu_at_original_build":"gpu_coverage_incomplete",
            (unsigned long long)probe->use.list,(unsigned long long)probe->use.creation,(unsigned long long)probe->use.generation,
            (unsigned long long)build.recording.list,(unsigned long long)build.recording.creation,
            (unsigned long long)build.recording.generation,(unsigned long long)build.order,
            (unsigned long long)build.instances.key.object,(unsigned long long)build.instances.key.creation,
            (unsigned long long)build.instances.offset,(unsigned long long)build.instances.bytes,unsigned(build.instances.heap),
            build.count,result.covered,probe->targets.count,result.total,stored,unsigned(covered&&result.total>stored),
            (unsigned long long)probe->completion.queue,(unsigned long long)probe->completion.fence,(unsigned long long)probe->completion.value,
            (unsigned long long)probe->state_at_build.pipeline,(unsigned long long)probe->state_at_build.root_signature,
            probe->state_at_build.pipeline_kind,(unsigned long long)copy.source.object,(unsigned long long)copy.source.creation,
            (unsigned long long)copy.source_offset,(unsigned long long)copy.destination_offset,(unsigned long long)copy.bytes,
            (unsigned long long)dispatch.call,(unsigned long long)dispatch.pipeline,(unsigned long long)dispatch.layout,
            dispatch.parameter,(unsigned long long)dispatch.uav_address,dispatch.x,dispatch.y,dispatch.z);
        reshade::log::message(reshade::log::level::info,line);
        for(unsigned i=0;i<probe->targets.count;++i){
            const auto &target=probe->targets.entries[i];
            std::snprintf(line,sizeof(line),"RT-RAY-INSTANCE-TARGET build_order=%llu authority=none "
                "native_source=%llx/%llu/%llx@%llx/%llu/%llu plan=%llu revision=%llu primitive=%llx/%llu+%llu",
                (unsigned long long)build.order,(unsigned long long)target.native_blas.object,
                (unsigned long long)target.native_blas.creation,(unsigned long long)target.address,
                (unsigned long long)target.native_pose.list,(unsigned long long)target.native_pose.creation,
                (unsigned long long)target.native_pose.generation,(unsigned long long)target.plan,(unsigned long long)target.revision,
                (unsigned long long)target.primitive.object,(unsigned long long)target.primitive.creation,
                (unsigned long long)target.primitive_offset);
            reshade::log::message(reshade::log::level::info,line);
        }
        for(unsigned i=0;i<stored;++i){
            const auto &row=result.rows[i];const auto *w=row.words;
            std::snprintf(line,sizeof(line),"RT-RAY-INSTANCE-ROW build_order=%llu index=%u authority=none "
                "raw64=%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x,%08x",
                (unsigned long long)build.order,row.index,w[0],w[1],w[2],w[3],w[4],w[5],w[6],w[7],w[8],w[9],w[10],w[11],w[12],w[13],w[14],w[15]);
            reshade::log::message(reshade::log::level::info,line);
        }
    }
