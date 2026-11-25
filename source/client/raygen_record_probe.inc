    void raygen_probe_status(unsigned bit,const char *status,const RaySceneInput &seen,Recording epoch,int rc=0){
        if(raygen_probe_reasons&bit)return;
        raygen_probe_reasons|=bit;
        const auto layout=ray_scene.raygen_probe_layout(seen.compute.root_signature);
        const auto closure=ray_scene.raygens.fixed_as_status({seen.compute.pipeline},seen.compute.root_signature,
            layout?layout->generation:0);
        char line[480];
        std::snprintf(line,sizeof(line),"RT-RAY-ID stage=%s authority=none rc=%d record=%llx/%llu/%llu "
            "state=%llx root=%llx/%llu default_gate=%s rg_alloc=%llx/%llu+%llu/%llu legacy_ready=%u",
            status,rc,(unsigned long long)epoch.list,(unsigned long long)epoch.creation,(unsigned long long)epoch.generation,
            (unsigned long long)seen.compute.pipeline,(unsigned long long)seen.compute.root_signature,
            (unsigned long long)(layout?layout->generation:0),closure,
            (unsigned long long)seen.raygen.key.object,(unsigned long long)seen.raygen.key.creation,
            (unsigned long long)seen.raygen.offset,(unsigned long long)seen.raygen.bytes,seen.compute.ready);
        reshade::log::message(reshade::log::level::info,line);
    }
    void record_raygen_probe(api::command_list *list,std::uint64_t native_list,const D3D12_DISPATCH_RAYS_DESC &dispatch){
        if(raygen_probe_taken.load(std::memory_order_relaxed)||!dispatch.Width||!dispatch.Height||!dispatch.Depth)return;
        const auto seen=ray_scene.read(inputs,list,dispatch.RayGenerationShaderRecord,dispatch.HitGroupTable,
            compute_query,read_as_slot,read_ray_as_slot);
        if(!seen||seen->stage!=RaySceneInput::raygen_upload||seen->raygen.heap!=api::memory_heap::default_)return;
        const auto epoch=inputs.recording(list);
        if(!epoch.list||!epoch.creation||!epoch.generation)return;
        const auto layout=ray_scene.raygen_probe_layout(seen->compute.root_signature);
        if(!layout||!layout->probe_data_supported()){
            raygen_probe_status(layout?2u:1u,layout?"root_tables_incompatible":"root_p4_unobserved",*seen,epoch);
            return;
        }
        auto *base=reinterpret_cast<ID3D12GraphicsCommandList *>(native_list);
        ComPtr<ID3D12GraphicsCommandList4> command;
        if(!base||FAILED(base->QueryInterface(IID_PPV_ARGS(&command)))||
           (command->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT&&command->GetType()!=D3D12_COMMAND_LIST_TYPE_COMPUTE))return;
        RaygenProbeBindings bindings;
        if(const auto rc=bindings.read(epoch.list,native_list,seen->compute.root_signature,capture_occlusion_compute,capture_occlusion_graphics)){
            raygen_probe_status(rc<20?4u:8u,rc<20?"graphics_tables_unrestorable":"compute_tables_unrestorable",*seen,epoch,rc);
            return;
        }
        const auto compute_flags=raygen_probe_root_flags(bindings.compute.root_signature);
        const auto graphics_flags=bindings.graphics.root_signature?raygen_probe_root_flags(bindings.graphics.root_signature):0;
        if(compute_flags||graphics_flags){
            const bool direct_indexed=compute_flags==2||graphics_flags==2;
            raygen_probe_status(direct_indexed?512u:256u,
                direct_indexed?"root_heap_direct_indexed":"root_flags_unavailable",*seen,epoch,10*compute_flags+graphics_flags);
            return;
        }
        std::shared_ptr<RaygenRecordProbe> probe;
        try{
            probe=std::make_shared<RaygenRecordProbe>();
            probe->recording=epoch;probe->state=seen->compute;probe->layout=*layout;
            probe->view=seen->raygen;probe->dispatch=dispatch;
            probe->catalog=ray_scene.raygens.record_catalog({seen->compute.pipeline},seen->compute.root_signature,layout->generation);
            if(probe->catalog.candidates.empty())return;
            if(!inputs.resource_current(probe->view.key)){
                raygen_probe_status(16,"source_retired",*seen,epoch);return;
            }
            // At most one private allocation attempt in the existing arm.
            raygen_probe_taken.store(true,std::memory_order_relaxed);
            if(!probe->prepare(native.Get(),bindings,read_asset(L"ReadRtRaygenId.cso"))){
                raygen_probe_status(16,"source_or_readback_unavailable",*seen,epoch);return;
            }
            // Only offsets proved by actual final-state entry metadata,
            // once per distinct slot. Save the real typed write now, not
            // a future lookup on a reused heap address after the GPU fence.
            std::uint64_t seen_slots=0;
            for(const auto &e:probe->catalog.candidates){
                if(!e.as_offset)continue;
                const auto bit=std::uint64_t(1)<<*e.as_offset;
                if(seen_slots&bit)continue;
                seen_slots|=bit;
                auto &slot=probe->slots.emplace_back();
                slot.offset=*e.as_offset;
                slot.status=read_ray_as_slot(epoch.list,&seen->compute,scene_srv_parameter,slot.offset,&slot.slot);
                if(slot.status)continue;
                if(slot.slot.version!=1||slot.slot.parameter!=scene_srv_parameter||slot.slot.table_offset!=slot.offset||
                   slot.slot.table!=bindings.original_as_table||!slot.slot.table||!slot.slot.heap_generation||!slot.slot.write){
                    slot.status=-1;continue;
                }
                if(slot.slot.location){
                    slot.allocation=inputs.resource_at(slot.slot.location);
                    if(!slot.allocation){slot.status=-2;continue;}
                    RaySceneInput use;
                    use.stage=RaySceneInput::bound;use.as=slot.slot;
                    slot.build=ray_scene.build_use(inputs,epoch,use);
                    if(slot.build&&slot.build->destination!=slot.allocation)slot.build.reset();
                }
            }
            if(!hold_draw_recording(epoch,list)){
                raygen_probe_status(32,"recording_unavailable",*seen,epoch);return;
            }
            records[epoch.list].push_back(probe);
        }catch(const std::exception &){
            raygen_probe_status(64,"private_prepare_failed",*seen,epoch);return;
        }
        // The host will now make the actual DispatchRays after this
        // synchronous callback returns 0. There is no SDK cancellation
        // between our read and that native call.
        probe->issue(command.Get(),bindings);
        raygen_probe=std::move(probe);
        raygen_probe_status(128,"gpu_recorded",*seen,epoch);
    }
    void settle_raygen_probe(const char *terminal=nullptr,int rc=0){
        const auto probe=raygen_probe;
        if(!probe)return;
        if(!terminal){
            if(probe->pre>1)terminal="replayed_before_readback";
            else if(!probe->pre&&inputs.recording(reinterpret_cast<api::command_list *>(probe->recording.list))!=probe->recording)
                terminal="recording_retired_without_submit";
            else if(probe->pre!=1||probe->post!=1||!writer_signal_known(probe->completion,true))return;
        }
        RaygenRecordProbe::Result result{};
        const RaygenIdentity::RecordCandidate *selected=nullptr;
        const RaygenProbeAs *slot=nullptr;
        const bool read=!terminal&&probe->read_result(result);
        const bool snapshot=read&&result.marker==RaygenRecordProbe::marker;
        if(!terminal){
            if(!snapshot)terminal=read?"no_gpu_read_receipt":"readback_map_failed";
            else{
                unsigned matches=0;
                for(const auto &c:probe->catalog.candidates)if(c.id==result.id){selected=&c;++matches;}
                if(matches!=1){selected=nullptr;terminal=matches?"ambiguous_identifier":"identifier_not_in_catalog";}
                else{
                    terminal=selected->pinned?"selected_pinned_identifier":"selected_native_identifier";
                    if(selected->as_offset){
                        const auto it=std::find_if(probe->slots.begin(),probe->slots.end(),[&](const auto &v){return v.offset==*selected->as_offset;});
                        if(it!=probe->slots.end())slot=&*it;
                    }
                }
            }
        }
        char id[sizeof(ShaderId)*2+1]{};
        if(snapshot){
            constexpr char hex[]="0123456789abcdef";
            const auto *bytes=reinterpret_cast<const unsigned char *>(result.id.data());
            for(size_t i=0;i<sizeof(ShaderId);++i){id[2*i]=hex[bytes[i]>>4];id[2*i+1]=hex[bytes[i]&15];}
        }
        const auto &epoch=probe->recording;const auto &view=probe->view;const auto &done=probe->completion;
        const auto &dispatch=probe->dispatch;
        char line[1600];
        std::snprintf(line,sizeof(line),"RT-RAY-ID stage=%s authority=none rc=%d record=%llx/%llu/%llu state=%llx root=%llx/%llu "
            "closure=%s raygens=%u resolved=%zu candidate_slots=%zu rg=%llx/%llu rg_alloc=%llx/%llu+%llu "
            "miss=%llx/%llu/%llu hit=%llx/%llu/%llu callable=%llx/%llu/%llu dims=%u,%u,%u "
            "id=%s export=%ls entry=%s queue=%llx fence=%llx/%llu pre=%u post=%u "
            "p4_offset=%u as_status=%d as_scope=cpu_at_native_record as=%llx as_alloc=%llx/%llu "
            "table=%llx heap_slot=%u heap_write=%llu/%llu source_slot=%u source_write=%llu/%llu p4_flags=%u",
            terminal,rc,(unsigned long long)epoch.list,(unsigned long long)epoch.creation,(unsigned long long)epoch.generation,
            (unsigned long long)probe->state.pipeline,(unsigned long long)probe->state.root_signature,(unsigned long long)probe->layout.generation,
            probe->catalog.closure,probe->catalog.declared,probe->catalog.candidates.size(),probe->slots.size(),
            (unsigned long long)dispatch.RayGenerationShaderRecord.StartAddress,(unsigned long long)view.bytes,
            (unsigned long long)view.key.object,(unsigned long long)view.key.creation,(unsigned long long)view.offset,
            (unsigned long long)dispatch.MissShaderTable.StartAddress,(unsigned long long)dispatch.MissShaderTable.SizeInBytes,
            (unsigned long long)dispatch.MissShaderTable.StrideInBytes,(unsigned long long)dispatch.HitGroupTable.StartAddress,
            (unsigned long long)dispatch.HitGroupTable.SizeInBytes,(unsigned long long)dispatch.HitGroupTable.StrideInBytes,
            (unsigned long long)dispatch.CallableShaderTable.StartAddress,(unsigned long long)dispatch.CallableShaderTable.SizeInBytes,
            (unsigned long long)dispatch.CallableShaderTable.StrideInBytes,dispatch.Width,dispatch.Height,dispatch.Depth,
            snapshot?id:"none",selected?selected->alias.c_str():L"unknown",selected?selected->entry.c_str():"unknown",
            (unsigned long long)done.queue,(unsigned long long)done.fence,(unsigned long long)done.value,probe->pre,probe->post,
            selected&&selected->as_offset?*selected->as_offset:UINT32_MAX,slot?slot->status:-3,
            (unsigned long long)(slot&&!slot->status?slot->slot.location:0),(unsigned long long)(slot?slot->allocation.object:0),
            (unsigned long long)(slot?slot->allocation.creation:0),(unsigned long long)(slot?slot->slot.table:0),
            slot?slot->slot.heap_slot:UINT32_MAX,(unsigned long long)(slot?slot->slot.heap_generation:0),
            (unsigned long long)(slot?slot->slot.write:0),slot?slot->slot.source_slot:UINT32_MAX,
            (unsigned long long)(slot?slot->slot.source_heap_generation:0),
            (unsigned long long)(slot?slot->slot.source_write:0),probe->layout.p4_flags);
        reshade::log::message(reshade::log::level::info,line);
        if(slot&&slot->build){
            const auto &b=*slot->build;const auto &i=b.instances;
            std::snprintf(line,sizeof(line),"RT-RAY-ID-BUILD authority=none consumer=%llx/%llu/%llu "
                "build=%llx/%llu/%llu/%llu build_native_submits_at_record=%u build_queue_at_record=%llx build_fence_at_record=%llx/%llu "
                "instances=%llx instance_alloc=%llx/%llu+%llu/%llu heap=%u count=%u flags=%u",
                (unsigned long long)epoch.list,(unsigned long long)epoch.creation,(unsigned long long)epoch.generation,
                (unsigned long long)b.recording.list,(unsigned long long)b.recording.creation,(unsigned long long)b.recording.generation,
                (unsigned long long)b.order,b.submissions,(unsigned long long)b.submit.queue,(unsigned long long)b.submit.fence,
                (unsigned long long)b.submit.value,(unsigned long long)b.instances_va,(unsigned long long)i.key.object,
                (unsigned long long)i.key.creation,(unsigned long long)i.offset,(unsigned long long)i.bytes,unsigned(i.heap),b.count,b.flags);
            reshade::log::message(reshade::log::level::info,line);
        }
        // Observation only. Recording and actual flights retain the
        // resources for every replay even after this first read settles.
        raygen_probe.reset();
    }
