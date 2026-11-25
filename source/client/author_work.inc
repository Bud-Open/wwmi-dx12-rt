    void observe_author_work(std::uint64_t deadline)noexcept{
        const auto now=GetTickCount64();
        if(author_work.end||!deadline||deadline<=now)return;
        // The existing visible window is 60 seconds. Only sample from
        // first observation onwards; late admission does not fill old bins.
        auto &work=author_work;
        constexpr auto window_ms=AuthorWorkWindow::bin_ms*AuthorWorkWindow::bin_count;
        work.start=deadline>=window_ms?deadline-window_ms:now;work.admitted=now;work.end=deadline;
        work.start_qpc=rt_cpu::now();
        LARGE_INTEGER q{};QueryPerformanceFrequency(&q);work.qpc_frequency=q.QuadPart;
        work.next_sample.fill(now);
        try{
            work.samples.reserve(64);
            for(const auto &[_,weak]:binding_recordings)if(const auto draw=weak.lock())
                for(const auto &prefix:draw->author_prefixes)note_work_output(prefix.output,nullptr,false);
        }catch(const std::bad_alloc &){}
        timed_device.store(native_device_id,std::memory_order_relaxed);
        timed_close_lists.store(0,std::memory_order_relaxed);
        for(auto &due:native_timing_due)due.store(now,std::memory_order_relaxed);
        timed_until.store(deadline,std::memory_order_release);
        char line[256];
        std::snprintf(line,sizeof(line),"RT-AUTHOR-WORK begin start_ms=%llu admitted_ms=%llu deadline_ms=%llu admitted_qpc=%llu qpc_frequency=%llu bin_ms=%u",
            (unsigned long long)work.start,(unsigned long long)now,(unsigned long long)work.end,
            (unsigned long long)work.start_qpc,(unsigned long long)work.qpc_frequency,AuthorWorkWindow::bin_ms);
        reshade::log::message(reshade::log::level::info,line);
    }
    void note_work_output(const std::shared_ptr<AuthorOutput> &out,AuthorWorkBin *bin,bool storage_reused)noexcept{
        const auto &pose=*out->pose;const auto &batch=*out->batch;
        const auto coverage=batch.single_component;
        if(bin){
            ++(coverage==UINT32_MAX?bin->created_full:bin->created_component);
            bin->storage_reuse+=storage_reused;
            if(!storage_reused){
                bin->position_bytes+=12ull*pose.pack->vertices;
                if(batch.tangents)bin->tangent_bytes+=32ull*pose.pack->vertices;
            }
        }
        auto &work=author_work;
        if(work.generation_entries>=AuthorWorkWindow::identity_limit){if(bin)++bin->identity_excluded;return;}
        try{
            // load/key/revision fix the exact immutable selected geometry and
            // part recipe. The owned route rejects external final/shape data.
            // Preserve actual source ranges, recording identities and write
            // orders; equal palettes and unequal pointers alone prove neither.
            std::vector<std::uint64_t> key{pose.pack->load_generation,pose.pack->key,pose.pack->revision,coverage,
                pose.recording.list,pose.recording.creation,pose.recording.generation,pose.content.object,pose.content.creation,pose.content_offset};
            key.reserve(key.size()+18*pose.source_orders.size());
            for(std::size_t i=0;i<pose.source_orders.size();++i){
                const auto order=pose.source_orders[i];const auto &v=pose.source_vertices[i];const auto &f=pose.final_positions[i];
                auto writer=pose.recording;
                if(const auto old=std::find_if(out->other_writers.begin(),out->other_writers.end(),[&](const auto &w){return w.first==order;});
                   old!=out->other_writers.end())writer=old->second;
                key.insert(key.end(),{order,writer.list,writer.creation,writer.generation,v.vertex.object,v.vertex.creation,v.offset,v.bytes,
                    f.mode,f.resource.object,f.resource.creation,f.address,f.bytes,f.first_vertex,f.vertex_count,
                    pose.native_delta_flags[i],std::uint64_t(pose.native_extra_input[i]),std::bit_cast<std::uint32_t>(pose.scales[i])});
            }
            auto &row=work.generations[std::move(key)];
            if(bin){
                const bool pending=std::any_of(row.outputs.begin(),row.outputs.end(),[&](const auto &weak){
                    const auto other=weak.lock();
                    if(!other||other->batch->gpu_recording==batch.gpu_recording)return false;
                    if(!other->submitted)return true;
                    const auto signal=other->production;const auto q=queues.find(signal.queue);
                    if(!signal.value||q==queues.end()||reinterpret_cast<std::uint64_t>(q->second.author_ready.Get())!=signal.fence||
                       q->second.author_completed>=signal.value)return false;
                    // Only an exact equivalent duplicate warrants a read of
                    // its existing fence when cached completion is older.
                    const auto completed=q->second.author_ready->GetCompletedValue();
                    return completed!=UINT64_MAX&&completed<signal.value;
                });
                bin->equivalent_pending+=pending;
                if(!row.created_here){++bin->distinct_created;row.created_here=true;}
            }
            row.outputs.push_back(out);++work.generation_entries;
        }catch(const std::bad_alloc &){if(bin)++bin->identity_excluded;}
    }
    static bool section_timestamp_outside(CaptureOcclusionGraphicsFn capture,std::uint64_t api_list,
        std::uint64_t native_list)noexcept{
        if(!capture||!api_list||!native_list)return false;
        WWMI_GraphicsRestoreInfo_v1 graphics{sizeof(graphics)};
        WWMI_GraphicsRestoreRecord_v1 tables[64];
        return capture(api_list,&graphics,tables,64)==0&&graphics.size==sizeof(graphics)&&graphics.version==1&&
            graphics.native_list==native_list&&graphics.outside_render_pass==1&&graphics.record_count<=64&&
            (graphics.level==WWMI_GRAPHICS_RESTORE_EMPTY||graphics.level==WWMI_GRAPHICS_RESTORE_TABLES);
    }
    std::shared_ptr<GpuWorkSample> begin_work_timer(GpuWorkKind kind,ID3D12GraphicsCommandList *list,
        Recording epoch,DrawBindingRecording *recording=nullptr,std::uint64_t call=0,std::uint64_t frame=0)noexcept{
        auto &work=author_work;
        if(!work.end||work.summary)return {};
        const auto now=GetTickCount64();const auto k=unsigned(kind);
        if(!work.at(now)||now<work.next_sample[k]||work.samples.size()>=64)return {};
        const auto required=kind==GpuWorkKind::native_compute?D3D12_COMMAND_LIST_TYPE_COMPUTE:D3D12_COMMAND_LIST_TYPE_DIRECT;
        if(list->GetType()!=required)return {};
        if(kind==GpuWorkKind::section&&
           !section_timestamp_outside(capture_occlusion_graphics,epoch.list,reinterpret_cast<std::uint64_t>(list))){
            // Retry another nearby section rather than probing every draw.
            work.next_sample[k]=now+AuthorWorkWindow::section_pass_retry_ms;
            ++work.section_pass_unproven;
            return {};
        }
        work.next_sample[k]=now+AuthorWorkWindow::timing_stride_ms;
        if(k>=unsigned(GpuWorkKind::native_direct))native_timing_due[k-unsigned(GpuWorkKind::native_direct)].store(work.next_sample[k],std::memory_order_relaxed);
        try{
            auto timer=std::make_shared<GpuWorkSample>();
            D3D12_QUERY_HEAP_DESC desc{D3D12_QUERY_HEAP_TYPE_TIMESTAMP,2,0};
            if(FAILED(native->CreateQueryHeap(&desc,IID_PPV_ARGS(&timer->query)))){++work.omitted[k];return {};}
            timer->readback=gpu::buffer(native.Get(),2*sizeof(std::uint64_t),D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
            timer->kind=kind;timer->epoch=epoch;timer->at_ms=now;timer->qpc=rt_cpu::now();
            timer->call=call;timer->frame=frame;timer->native_list=reinterpret_cast<std::uint64_t>(list);
            timer->id=work.samples.size()+1;
            work.samples.push_back(timer);
            if(recording){
                try{recording->timers.push_back(timer);}catch(...){work.samples.pop_back();throw;}
                timed_close_lists.fetch_or(timed_list_bit(epoch.list),std::memory_order_release);
            }
            // Prefix lists are owned; native Dispatch has its host receipt.
            // A private section additionally proved graphics-pass state above.
            list->EndQuery(timer->query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0);
            return timer;
        }catch(const std::exception &){++work.omitted[k];return {};}
    }
    static void end_work_timer(GpuWorkSample &timer,ID3D12GraphicsCommandList *list)noexcept{
        if(timer.ended||timer.native_list!=reinterpret_cast<std::uint64_t>(list))return;
        list->EndQuery(timer.query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,1);timer.ended=true;
    }
    static void resolve_work_timer(GpuWorkSample &timer,ID3D12GraphicsCommandList *list)noexcept{
        if(!timer.ended||timer.resolved||timer.pre||timer.reported||timer.native_list!=reinterpret_cast<std::uint64_t>(list))return;
        list->ResolveQueryData(timer.query.Get(),D3D12_QUERY_TYPE_TIMESTAMP,0,2,timer.readback.Get(),0);
        timer.resolved=true;
    }
    void close_work_timers(api::command_list *list){
        const auto draw=draw_recording(reinterpret_cast<std::uint64_t>(list));
        if(!draw||draw->timers.empty()||draw->epoch!=inputs.recording(list))return;
        const auto reset=draw->reset.load(std::memory_order_acquire);
        if(reset==DrawBindingRecording::waiting||reset==DrawBindingRecording::succeeded)return;
        // Host callback is before native Close; a valid list is outside
        // Begin/EndRenderPass. Never ResolveQueryData in the draw callback.
        auto *native_list=reinterpret_cast<ID3D12GraphicsCommandList *>(list->get_native());
        for(const auto &timer:draw->timers)resolve_work_timer(*timer,native_list);
    }
    static void set_work_signal(GpuWorkSample &timer,Queue &q,NativeIdentityFence signal,bool producer)noexcept{
        if(!q.timestamp_frequency_checked){
            q.timestamp_frequency_checked=true;
            UINT64 hz=0;if(SUCCEEDED(q.native->GetTimestampFrequency(&hz)))q.timestamp_frequency=hz;
        }
        timer.signal=signal;timer.producer_fence=producer;timer.frequency=q.timestamp_frequency;
    }
    std::uint64_t start_native_timer(api::command_list *list,const WWMI_RtNativeDispatch_v2 &event,GpuWorkKind kind)noexcept{
        try{
            if(!author_work.current())return 0;
            const auto epoch=inputs.recording(list);
            if(!epoch.list||!epoch.creation||!epoch.generation)return 0;
            const auto draw=hold_draw_recording(epoch,list);if(!draw)return 0;
            auto *command=reinterpret_cast<ID3D12GraphicsCommandList *>(event.native_list);
            const auto timer=begin_work_timer(kind,command,epoch,draw.get(),event.call_id);
            if(!timer)return 0;
            timer->work=std::uint64_t(event.group_x)*event.group_y*event.group_z;
            return timer->id;
        }catch(const std::exception &){return 0;}
    }
    void end_native_timer(const WWMI_RtNativeDispatch_v2 &event,std::uint64_t id)noexcept{
        if(!id||id>author_work.samples.size())return;
        auto &timer=*author_work.samples[id-1];
        if(timer.call==event.call_id&&timer.epoch.list==event.api_list)
            end_work_timer(timer,reinterpret_cast<ID3D12GraphicsCommandList *>(event.native_list));
    }
    const AuthorStreamPool::Storage *work_storage(std::uint64_t batch)const noexcept{
        if(!batch)return nullptr;
        for(const auto &storage:author_stream_pool.storage)if(const auto resident=storage->resident.lock();
            reinterpret_cast<std::uint64_t>(resident.get())==batch)return storage.get();
        return nullptr;
    }
    static void work_placements(GpuWorkSample &timer,const AuthorStreamPool::Storage *current,const AuthorStreamPool::Storage *previous=nullptr)noexcept{
        if(current){
            timer.current=current->generation;timer.current_coverage=current->coverage;timer.current_placement=current->default_static?2:1;
        }
        if(previous){
            timer.previous=previous->generation;timer.previous_coverage=previous->coverage;timer.previous_placement=previous->default_static?2:1;
        }
    }
    std::uint64_t observe_section_work(const WWMI_RtAuthorCommit_v1 &q,std::uint64_t deadline,std::uint64_t frame,
        unsigned phase,std::uint64_t id,unsigned parts,std::uint64_t indices)noexcept{
        observe_author_work(deadline);
        if(!author_work.end||q.disposition!=WWMI_RT_AUTHOR_PRIVATE||q.empty_selection||!q.current_batch)return 0;
        const auto binding=draw_recording(q.list);
        if(!binding||binding->epoch!=Recording{q.list,q.creation,q.recording}||
           binding->author_capture.snapshot.draw_ticket!=q.draw_ticket||!binding->committed_sections)return 0;
        const auto reset=binding->reset.load(std::memory_order_acquire);
        if(reset==DrawBindingRecording::waiting||reset==DrawBindingRecording::succeeded)return 0;
        auto *list=reinterpret_cast<ID3D12GraphicsCommandList *>(binding->native_list);
        if(phase&WWMI_RT_SECTION_WORK_BEFORE){
            const auto timer=begin_work_timer(GpuWorkKind::section,list,binding->epoch,binding.get(),q.draw_ticket,frame);
            return timer?timer->id:0;
        }
        if(!(phase&WWMI_RT_SECTION_WORK_AFTER)||!parts||!indices)return 0;
        const auto *current=work_storage(q.current_batch);
        const auto *previous=q.previous_batch?work_storage(q.previous_batch):nullptr;
        if(id&&id<=author_work.samples.size()){
            auto &timer=*author_work.samples[id-1];
            if(timer.kind==GpuWorkKind::section&&timer.epoch==binding->epoch&&timer.call==q.draw_ticket){
                work_placements(timer,current,previous);
                timer.parts=parts;timer.work=indices;end_work_timer(timer,list);
            }
        }
        if(auto *bin=author_work.current()){
            ++binding->observed_sections;
            ++bin->sections;bin->parts+=parts;bin->indices+=indices;
            if(!bin->first_frame){
                bin->first_frame=frame;bin->first_current=current?current->generation:0;bin->first_previous=previous?previous->generation:0;
            }
            bin->last_frame=frame;
            if(current)++(current->default_static?bin->current_default:bin->current_bootstrap);
            else ++bin->current_unclassified;
            if(previous)++(previous->default_static?bin->previous_default:bin->previous_bootstrap);
            else bin->previous_unclassified+=q.previous_batch!=0;
        }
        return 0;
    }
    void snapshot_author_work(AuthorWorkBin &bin,std::uint64_t now){
        // Asset widths are the exact populated static buffers, not heap
        // reservations; a pending upload can temporarily own two copies.
        for(const auto &[_,asset]:author_static_assets){
            const auto &a=*asset;
            const unsigned state=!a.bootstrap?0:!a.geometry||(a.producer&&!a.signalled)?3:a.producer?2:1;
            const auto &g=*(a.bootstrap?a.bootstrap:a.geometry);const auto &p=*g.pack;
            ++bin.asset_count[state];
            bin.asset_bytes[state]+=p.positions.size()+p.blends.size()+p.indices.size()+
                (g.vectors?p.vectors.size():0)+(g.uv?p.uv.size():0)+(g.color?p.color.size():0);
        }
        std::map<const GeometryBatch *,unsigned> owners;
        const auto draws=[&](const auto &weak,unsigned bit){
            if(const auto draw=weak.lock())for(auto *batch:draw->author_batches)owners[batch]|=bit;
        };
        for(const auto &[_,weak]:binding_recordings)if(const auto draw=weak.lock()){
            draws(weak,1);
            for(const auto &prefix:draw->author_prefixes)bin.uncommitted_live+=!prefix.committed;
        }
        for(const auto &[_,q]:queues)for(const auto &flight:q.flights)draws(flight.draw,2);
        const auto history=[&](const auto &outputs){
            if(!outputs)return;
            const auto note=[&](const auto &o){if(o)owners[o->batch.get()]|=4;};
            note(outputs->full);for(const auto &part:outputs->parts)note(part);
        };
        for(const auto &h:surface_history)history(h.author_outputs);
        for(const auto &p:previous_prepared)history(p.author_outputs);
        for(const auto &cmd:author_commands)if(cmd->producing)owners[cmd->producing.get()]|=8;
        for(const auto &storage:author_stream_pool.storage){
            const auto batch=storage->resident.lock();const auto owner=batch?owners[batch.get()]:0;
            const unsigned slot=!batch?5:owner&1?0:owner&2?1:owner&4?2:owner&8?3:4;
            bin.owner_bytes[slot]+=storage->position_capacity+storage->tangent_capacity;
        }
        bin.inventory_at=now;bin.inventory=true;
    }
    void report_author_work(bool stopping)noexcept{
        auto &work=author_work;
        if(!work.end||work.timing_summary)return;
        const auto now=GetTickCount64();
        if(!work.summary)if(auto *bin=work.at(now)){
            const auto index=int(bin-work.bins.data());
            if(index!=work.last_inventory){
                work.last_inventory=index;
                try{snapshot_author_work(*bin,now);}catch(const std::bad_alloc &){}
            }
        }
        using U=unsigned long long;
        char line[2048];
        if(!work.summary&&(stopping||now>=work.end)){
            work.summary=true;timed_until.store(0,std::memory_order_release);
            const auto until=(std::min)(now,work.end);
            for(unsigned i=0;i<work.bins.size();++i){
                const auto start=work.start+std::uint64_t(i)*AuthorWorkWindow::bin_ms;if(start>=until)break;
                const auto end=(std::min)(until,start+AuthorWorkWindow::bin_ms);
                const auto observed_from=(std::max)(start,work.admitted);
                const auto observed_ms=end>observed_from?end-observed_from:0;
                const auto coverage=observed_ms==0?"unobserved":observed_ms==AuthorWorkWindow::bin_ms?"full":"partial";
                const auto &b=work.bins[i];
                std::snprintf(line,sizeof(line),
                    "RT-AUTHOR-WORK bin=%u begin_ms=%llu end_ms=%llu first_frame=%llu last_frame=%llu created_full=%llu created_component=%llu distinct_created=%llu identity_excluded=%llu equivalent_pending=%llu storage_reuse=%llu new_position_bytes=%llu new_tangent_bytes=%llu local_acquire=%llu cross_ready=%llu uncommitted_reset=%llu prefix_submit_full=%llu prefix_submit_component=%llu prefix_vertices=%llu commands_retired=%llu sections=%llu parts=%llu indices=%llu current_default=%llu current_bootstrap=%llu previous_default=%llu previous_bootstrap=%llu current_unknown=%llu previous_unknown=%llu first_current_output=%llu first_previous_output=%llu consumer_executes=%llu consumer_commit_sections=%llu consumer_observed_sections=%llu consumer_replays=%llu observed_ms=%llu coverage=%s",
                    i,U(start),U(end),U(b.first_frame),U(b.last_frame),
                    U(b.created_full),U(b.created_component),U(b.distinct_created),U(b.identity_excluded),U(b.equivalent_pending),U(b.storage_reuse),U(b.position_bytes),U(b.tangent_bytes),
                    U(b.local_acquire),U(b.cross_ready),U(b.uncommitted_reset),U(b.prefix_submit_full),U(b.prefix_submit_component),U(b.prefix_vertices),U(b.command_retired),
                    U(b.sections),U(b.parts),U(b.indices),U(b.current_default),U(b.current_bootstrap),U(b.previous_default),U(b.previous_bootstrap),U(b.current_unclassified),U(b.previous_unclassified),
                    U(b.first_current),U(b.first_previous),U(b.consumer_executes),U(b.consumer_sections),U(b.consumer_recorded_in_window),U(b.consumer_replays),
                    U(observed_ms),coverage);
                reshade::log::message(reshade::log::level::info,line);
                if(b.inventory){
                    std::snprintf(line,sizeof(line),
                        "RT-AUTHOR-OWNERS bin=%u at_ms=%llu default_count=%u bootstrap_count=%u pending_count=%u bootstrap_failed_count=%u default_bytes=%llu bootstrap_bytes=%llu pending_bytes=%llu bootstrap_failed_bytes=%llu uncommitted_live=%u recording_bytes=%llu flight_only_bytes=%llu history_only_bytes=%llu producer_only_bytes=%llu other_owned_bytes=%llu idle_pool_bytes=%llu",
                        i,U(b.inventory_at),b.asset_count[0],b.asset_count[1],b.asset_count[2],b.asset_count[3],U(b.asset_bytes[0]),U(b.asset_bytes[1]),U(b.asset_bytes[2]),U(b.asset_bytes[3]),b.uncommitted_live,
                        U(b.owner_bytes[0]),U(b.owner_bytes[1]),U(b.owner_bytes[2]),U(b.owner_bytes[3]),U(b.owner_bytes[4]),U(b.owner_bytes[5]));
                    reshade::log::message(reshade::log::level::info,line);
                }
            }
            work.generations.clear();
        }
        const bool final=stopping||now>=work.end+2000;
        if(work.summary&&!stopping){
            rt_phase_cpu::close(false);
            rt_phase_cpu::report("client",work.start,rt_phase_cpu::runtime_wait,rt_phase_cpu::descriptor_copy,final);
        }
        for(const auto &sample:work.samples){
            auto &s=*sample;if(s.reported)continue;
            const char *reason=s.error;
            std::uint64_t ticks[2]{};
            if(!reason&&s.pre>1)reason="replay_before_readback";
            if(!reason&&s.signal.value){
                const auto q=queues.find(s.signal.queue);
                if(q==queues.end())reason="missing_queue";
                else{
                    const auto &owner=q->second;
                    const auto *fence=s.producer_fence?owner.author_ready.Get():owner.fence.Get();
                    const auto completed=s.producer_fence?owner.author_completed:owner.completed;
                    if(reinterpret_cast<std::uint64_t>(fence)!=s.signal.fence)reason="fence_mismatch";
                    else if(completed==UINT64_MAX)reason="device_removed";
                    else if(completed<s.signal.value){if(!final)continue;reason="completion_not_observed";}
                    else if(!s.frequency)reason="frequency_unavailable";
                    else if(!s.resolved)reason=s.ended?"close_not_resolved":"end_not_recorded";
                    else{
                        void *mapped=nullptr;const D3D12_RANGE read{0,sizeof(ticks)},written{0,0};
                        if(FAILED(s.readback->Map(0,&read,&mapped)))reason="readback_map_failed";
                        else{std::memcpy(ticks,mapped,sizeof(ticks));s.readback->Unmap(0,&written);if(ticks[1]<ticks[0])reason="timestamp_order";}
                    }
                }
            }else if(!reason){
                if(!final)continue;
                reason=s.pre?"signal_not_observed":s.resolved?"never_submitted":s.ended?"close_not_observed":"end_not_observed";
            }
            s.reported=true;
            const double us=reason?0:double(ticks[1]-ticks[0])*1e6/double(s.frequency);
            std::snprintf(line,sizeof(line),
                "RT-GPU-WORK sample=%llu kind=%s status=%s at_ms=%llu qpc=%llu frame=%llu list=%llx record=%llu/%llu queue=%llx fence=%llx value=%llu frequency=%llu start_tick=%llu end_tick=%llu gpu_us=%.3f current_output=%llu previous_output=%llu current_coverage=%u previous_coverage=%u current_placement=%u previous_placement=%u parts=%u work=%llu",
                U(s.id),gpu_work_names[unsigned(s.kind)],reason?reason:"ready",U(s.at_ms),U(s.qpc),U(s.frame),U(s.native_list),U(s.epoch.creation),U(s.epoch.generation),
                U(s.signal.queue),U(s.signal.fence),U(s.signal.value),U(s.frequency),U(ticks[0]),U(ticks[1]),us,U(s.current),U(s.previous),
                s.current_coverage,s.previous_coverage,s.current_placement,s.previous_placement,s.parts,U(s.work));
            reshade::log::message(reshade::log::level::info,line);
        }
        if(final){
            work.timing_summary=true;
            timed_until.store(0,std::memory_order_release);timed_close_lists.store(0,std::memory_order_release);
            std::snprintf(line,sizeof(line),"RT-GPU-WORK end samples=%u section_pass_unproven=%u allocation_omitted_prefix=%u allocation_omitted_section=%u allocation_omitted_direct=%u allocation_omitted_compute=%u",
                unsigned(work.samples.size()),work.section_pass_unproven,work.omitted[0],work.omitted[1],work.omitted[2],work.omitted[3]);
            reshade::log::message(reshade::log::level::info,line);
            work.samples.clear(); // recordings/flights still own any native query instructions
        }
    }
