// Runtime-owned, under Runtime::mutex. No source upload or original GPU
// resource is read by a closed author prefix.
    bool producer_recording_open(Recording rec){
        if(!rec.list||!rec.creation||!rec.generation)return false;
        if(inputs.recording(reinterpret_cast<api::command_list *>(rec.list))!=rec)return false;
        if(const auto prod_binding=draw_recording(rec.list)){
            const auto reset=prod_binding->reset.load(std::memory_order_acquire);
            if(reset==DrawBindingRecording::waiting||reset==DrawBindingRecording::succeeded)return false;
        }
        return true;
    }
    bool read_author_snapshot(const WWMI_RtPackDrawQuery_v2 &query,WWMI_RtAuthorSnapshot_v1 &out,
        NativeDrawIdentityCache::Match &match,WWMI_RtDrawIdentityReceipt_v1 *receipt=nullptr){
        if(!author_path||(!receipt&&!native_draw_candidate(query)))return false;
        if(!inputs.identify_draw_v2(query,out.binding,nullptr,0,&match,receipt,true))return false;
        const auto &id=*match.identity;
        const auto consumer=inputs.recording(reinterpret_cast<api::command_list *>(query.list));
        const bool same_open=(id.recording==consumer&&match.submissions==0);
        const bool submitted_once=(match.submissions==1&&writer_signal_known(match.producer,false));
        const bool parallel_open=(id.recording!=consumer&&match.submissions==0&&producer_recording_open(id.recording));
        if(!same_open&&!submitted_once&&!parallel_open){
            if(receipt)receipt->reason=WWMI_RT_IDENTIFY_ORDER_PENDING;
            return false;
        }
        if(receipt)receipt->reason=WWMI_RT_IDENTIFY_OK;
        out.struct_size=sizeof(out);out.abi_version=1;out.token=match.snapshot;
        out.source={consumer.creation,consumer.generation,id.allocation.object,id.allocation.creation,id.native_blas,id.components[match.component].sample.order};
        return true;
    }
    std::shared_ptr<DrawBindingRecording> hold_draw_recording(Recording epoch,api::command_list *list){
        auto found=draw_recording(epoch.list);
        if(!found){
            found=std::make_shared<DrawBindingRecording>();found->epoch=epoch;found->native_list=list->get_native();
            records[epoch.list].push_back(found);binding_recordings[epoch.list]=found;
        }
        const auto reset=found->reset.load(std::memory_order_acquire);
        return found->epoch==epoch&&reset!=DrawBindingRecording::waiting&&reset!=DrawBindingRecording::succeeded?found:nullptr;
    }
    bool capture_author_draw(const WWMI_RtPackDrawQuery_v2 &query,WWMI_RtAuthorSnapshot_v1 &out,
        WWMI_RtDrawIdentityReceipt_v1 *receipt){
        NativeDrawIdentityCache::Match match;
        if(!read_author_snapshot(query,out,match,receipt))return false;
        auto *list=reinterpret_cast<api::command_list *>(query.list);
        const auto recording=hold_draw_recording({query.list,out.source.list_creation,out.source.recording},list);
        if(!recording){if(receipt)receipt->reason=WWMI_RT_IDENTIFY_RECORDING;out={};return false;}
        out.draw_ticket=++next_author_draw;
        recording->author_capture={query,out,std::move(match),{}};
        history_owners_changed();
        return true;
    }
    AuthorCapture *captured_author(const WWMI_RtPackDrawQuery_v2 &query,std::uint64_t ticket){
        if(!ticket)return nullptr;
        const auto recording=draw_recording(query.list);
        if(!recording||recording->epoch!=inputs.recording(reinterpret_cast<api::command_list *>(query.list)))return nullptr;
        const auto reset=recording->reset.load(std::memory_order_acquire);
        if(reset==DrawBindingRecording::waiting||reset==DrawBindingRecording::succeeded)return nullptr;
        auto &value=recording->author_capture;
        const auto view=[](const auto &q){return std::tie(q.device,q.list,q.index_format,q.vertex_stride,q.index_count,q.first_index,
            q.base_vertex,q.vertex_buffer_gpu_va,q.vertex_buffer_bytes,q.index_buffer_gpu_va,q.index_buffer_bytes);};
        return value.snapshot.draw_ticket==ticket&&view(value.query)==view(query)?&value:nullptr;
    }
    static bool owned_author_pack(const std::shared_ptr<const ModPack> &pack){
        return pack&&!pack->shapekey_vertices&&pack->rest_skinning_known&&std::all_of(pack->components.begin(),pack->components.end(),[](const auto &part){
            return part.selection_planned&&part.mod_author_shared&&!part.extra_position_input;
        });
    }
    static bool planned_shared_author_pack(const std::shared_ptr<const ModPack> &pack){
        return pack&&std::all_of(pack->components.begin(),pack->components.end(),[](const auto &part){
            return part.selection_planned&&part.mod_author_shared;
        });
    }
#include "author_completed_pose.inc"
    SurfaceHistory *history_for(const std::shared_ptr<const NativeBlasIdentity> &id,const std::shared_ptr<const ModPack> &pack,
        std::uint64_t completed_plan=0){
        const auto it=std::find_if(surface_history.begin(),surface_history.end(),[&](const auto &h){
            return h.identity==id&&h.pack==pack&&h.completed_plan==completed_plan;
        });
        return it==surface_history.end()?nullptr:&*it;
    }
    SurfaceHistory &author_history_for(const std::shared_ptr<const NativeBlasIdentity> &id,
                                      const std::shared_ptr<const ModPack> &pack,NativeIdentityFence writer,
                                      std::uint64_t completed_plan=0){
        if(auto *h=history_for(id,pack,completed_plan))return *h;
        if(const auto *native=history_for(id,{});native&&native->native_signal.fence)writer=native->native_signal;
        // A native writer can feed REST and independently selected completed
        // recipes. Preserve each Pose, its batches, and any held previous use.
        surface_history.push_back({id,{},{},pack,{},writer,{},completed_plan});
        return surface_history.back();
    }
    AuthorPlan *find_author_plan(std::uint64_t plan){
        const auto it=std::find_if(author_plans.begin(),author_plans.end(),[&](const auto &p){return p->id==plan;});
        return it==author_plans.end()?nullptr:it->get();
    }
    std::shared_ptr<AuthorPlan> author_plan(const WWMI_RtAuthorScopeRequest_v1 &request,const WWMI_RtAuthorSnapshot_v1 &snapshot,
        ResourceKey primitive,const NativeDrawIdentityCache::Match &native_match,bool requires_source){
        auto it=std::find_if(author_plans.begin(),author_plans.end(),[&](const auto &p){
            return p->requires_source==requires_source&&p->snapshot==snapshot.token&&p->primitive==primitive&&
                p->load==request.load_generation&&p->mod==request.mod_key&&
                p->scope.byte_origin==request.instance.byte_origin&&
                !std::memcmp(p->scope.primitive_words,request.instance.primitive_words,sizeof(p->scope.primitive_words));
        });
        if(it!=author_plans.end())return *it;
        auto plan=std::make_shared<AuthorPlan>();plan->id=++next_author_plan;plan->snapshot=snapshot.token;
        plan->load=request.load_generation;plan->mod=request.mod_key;plan->requires_source=requires_source;
        plan->primitive=primitive;plan->scope=request.instance;plan->identity=native_match.identity;
        auto &refs=author_primitive_refs[{primitive.object,primitive.creation}];
        try{author_plans.push_back(plan);}catch(...){
            if(!refs)author_primitive_refs.erase({primitive.object,primitive.creation});
            throw;
        }
        ++refs;return plan;
    }
    WWMI_RtDeformedSelection_v1 deformed_owner(const AuthorPlan &plan,unsigned component)const{
        const auto &s=plan.identity->components[component].sample;
        return {native_device_id,plan.id,plan.snapshot,plan.primitive.object,plan.primitive.creation,plan.scope.byte_origin,
            plan.load,plan.mod,plan.pack->revision,0,
            {{s.recording.list,s.recording.creation,s.recording.generation},s.output.object,s.output.creation,
                s.output_offset,s.output_bytes,s.order,s.component,0},0,0};
    }
    bool scene_sources_sealed(const AuthorPlan &plan)const{
        if(!plan.requires_source||plan.refused_native||!plan.pack||!plan.identity||
           plan.sources.size()!=plan.pack->components.size()||plan.sources.size()!=plan.identity->components.size())return false;
        for(unsigned i=0;i<plan.sources.size();++i){
            const auto &source=plan.sources[i];
            if(!source.section||source.empty!=plan.pack->components[i].draws.empty())return false;
            auto expected=deformed_owner(plan,i);
            if(!source.empty){
                if(source.selection.kind!=WWMI_RT_DEFORMED_AUTHOR_REST||!source.selection.recipe_receipt)return false;
                expected.kind=source.selection.kind;expected.recipe_receipt=source.selection.recipe_receipt;
            }
            if(std::memcmp(&expected,&source.selection,sizeof(expected)))return false;
        }
        return true;
    }
    bool acquire_author_rest(const AuthorPlan &plan,const NativeDrawIdentityCache::Match &match,Recording consumer,
        const WWMI_RtDeformedSelection_v1 &selected)const{
        if(!mod_acquire_deformed||selected.kind!=WWMI_RT_DEFORMED_AUTHOR_REST||!selected.recipe_receipt)return false;
        auto expected=deformed_owner(plan,match.component);
        expected.kind=selected.kind;expected.recipe_receipt=selected.recipe_receipt;
        if(std::memcmp(&expected,&selected,sizeof(expected)))return false;
        const WWMI_RtDeformedSourceRequest_v1 query{sizeof(query),1,expected,{consumer.list,consumer.creation,consumer.generation}};
        WWMI_RtDeformedSource_v1 result{};
        if(mod_acquire_deformed(&query,sizeof(query),&result,sizeof(result))!=WWMI_RT_MOD_OK)return false;
        // Every nonzero success field transfers an independent lease. REST
        // has no GPU views or fence and never borrows a game position.
        ComPtr<ID3D12Resource> primary,vectors;ComPtr<ID3D12Fence> fence;
        primary.Attach(reinterpret_cast<ID3D12Resource *>(result.primary.resource));
        vectors.Attach(reinterpret_cast<ID3D12Resource *>(result.vectors.resource));
        fence.Attach(reinterpret_cast<ID3D12Fence *>(result.completion_fence));
        return result.struct_size==sizeof(result)&&result.abi_version==1&&
            !std::memcmp(&expected,&result.selection,sizeof(expected))&&!primary&&!vectors&&!fence&&
            !result.vertex_count&&!result.producer.list&&!result.producer_write_order&&!result.completion_value;
    }
    // Only frozen plan creation and first real NATIVE/PRIVATE writes call
    // this receipt. Keep distinct budgets so warm native refusal (including
    // an actual CURRENT status) cannot be consumed by normal successes.
    struct AuthorNoteKey {
        uint64_t load=0,program=0;
        unsigned component=0,classification=0,site=0,reason=0,surface=0,phase=0,geometry=0;
        bool operator==(const AuthorNoteKey&)const=default;
    };
    struct AuthorNoteBucket {
        std::array<AuthorNoteKey,24> seen{};
        unsigned used=0;
        bool announced=false;
    };
    struct AuthorNoteProfile {
        uint32_t guid=0,hash=0;
        std::array<AuthorNoteBucket,8> bucket{};
    };
    std::array<AuthorNoteProfile,16> author_note_profiles{};
    bool author_note_profile_capped=false;

    void note_author_choice(const AuthorPlan &plan,uint64_t ticket,uint64_t recording,unsigned component,
        bool freeze,unsigned from,const RtAuthorCommitNote* note,bool plan_violation=false) noexcept{
        const auto& profile=plan.identity->components.front().sample.profile;
        if(!profile)return;
        const auto reason=note?note->reason:0u, site=note?unsigned(note->site):0u;
        const unsigned surface=note?note->surface:0,phase=note?note->phase:0,geometry=note?note->geometry:0;
        const uint64_t program=note?note->program:0;
        const auto finish_cause=note&&note->site==RtAuthorChoiceSite::finish
            ?static_cast<RtAuthorFinishReason>(reason):RtAuthorFinishReason::none;
        unsigned kind;
        if(plan_violation)kind=7;
        else if(freeze)kind=plan.disposition==WWMI_RT_AUTHOR_TENTATIVE?0:1;
        else if(plan.disposition==WWMI_RT_AUTHOR_PRIVATE)kind=reason?3:2;
        else if(note&&note->site==RtAuthorChoiceSite::section_refusal&&reason==5)kind=4;
        else if(finish_cause==RtAuthorFinishReason::frame_result||finish_cause==RtAuthorFinishReason::frame_owner||
                finish_cause==RtAuthorFinishReason::incoherent_parts)kind=5;
        else kind=6;
        constexpr const char* kinds[]{"freeze-ready","freeze-refused","private","masked-first-private",
            "masked-refused","frame-failure","other-native","plan-violation"};
        constexpr const char* causes[]{"no-recipe-revision","catalog-pack-absent","pack-not-author",
            "no-surface","component-set","native-cohort","pose-assembly","rest-candidate","external-final"};
        const auto emit=[](const char* row){reshade::log::message(reshade::log::level::info,row);};
        char row[768];
        AuthorNoteProfile* found=nullptr;
        for(auto& p:author_note_profiles){
            if(!p.guid){p.guid=profile->object_guid;p.hash=profile->content_hash;}
            if(p.guid==profile->object_guid&&p.hash==profile->content_hash){found=&p;break;}
        }
        if(!found){
            if(!author_note_profile_capped){
                author_note_profile_capped=true;
                emit("RT-AUTHOR-CHOICE event=limit category=profiles cap=16");
            }
            return;
        }
        auto& bucket=found->bucket[kind];
        const AuthorNoteKey key{plan.load,program,component,freeze?unsigned(plan.freeze_cause):from,site,reason,surface,phase,geometry};
        if(std::find_if(bucket.seen.begin(),bucket.seen.begin()+bucket.used,[&](const AuthorNoteKey &v){return v==key;})!=bucket.seen.begin()+bucket.used)return;
        if(bucket.used==bucket.seen.size()){
            if(!bucket.announced){
                bucket.announced=true;
                std::snprintf(row,sizeof(row),"RT-AUTHOR-CHOICE event=limit profile=%08x/%08x category=%s cap=%zu",
                    found->guid,found->hash,kinds[kind],bucket.seen.size());
                emit(row);
            }
            return;
        }
        bucket.seen[bucket.used++]=key;
        std::snprintf(row,sizeof(row),"RT-AUTHOR-CHOICE event=%s category=%s profile=%08x/%08x plan=%llu snapshot=%llu ticket=%llu "
            "recording=%llu component=%u load=%llu mod=%llu revision=%llu from=%u to=%u initial=%u freeze=%s freeze_ticket=%llu "
            "freeze_recording=%llu freeze_component=%u site=%u reason=%u vs=%llx surface=%u phase=%u geometry=%u",
            freeze?"freeze":"commit",kinds[kind],found->guid,found->hash,(unsigned long long)plan.id,
            (unsigned long long)plan.snapshot,(unsigned long long)ticket,(unsigned long long)recording,component,
            (unsigned long long)plan.load,(unsigned long long)plan.mod,(unsigned long long)plan.freeze_revision,
            from,plan.disposition,plan.initial_disposition,causes[unsigned(plan.freeze_cause)],(unsigned long long)plan.freeze_ticket,
            (unsigned long long)plan.freeze_recording,plan.freeze_component,site,reason,(unsigned long long)program,surface,phase,geometry);
        emit(row);
    }
    bool begin_author_scope(const WWMI_RtAuthorScopeRequest_v1 &request,WWMI_RtAuthorScope_v1 &out,
        WWMI_RtDeformedSelection_v1 *deformation_owner=nullptr){
        auto *captured=captured_author(request.native_draw,request.draw_ticket);
        if(!captured||captured->snapshot.token!=request.snapshot)return false;
        auto &match=captured->match;
        {
            unsigned live_submissions=match.submissions;
            NativeIdentityFence live_producer=match.producer;
            if(identities.live_state(match.identity,live_submissions,live_producer)&&
               live_submissions<=1&&(match.submissions==0||live_submissions==1)){
                match.submissions=live_submissions;
                match.producer=live_producer;
            }
        }
        const auto primitive=inputs.primitive_key(request.instance.resource,request.instance.byte_origin);
        if(!primitive)return false;
        captured->plan=author_plan(request,captured->snapshot,primitive,match,deformation_owner!=nullptr);
        history_owners_changed();
        auto &plan=*captured->plan;
        if(!plan.choice_checked){
            // Freeze a complete choice before this scope can suppress any
            // native pass. A still-learning scope may finish the VM and
            // publish a recipe, but cannot switch halfway through this pose.
            const auto revision=mod_author_revision(plan.load,plan.mod);
            plan.freeze_revision=revision;
            if(revision){
                plan.freeze_cause=AuthorFreezeCause::catalog_pack_absent;
                const auto chosen=[&](){return std::find_if(catalog.packs.begin(),catalog.packs.end(),[&](const auto &p){
                    return p->load_generation==plan.load&&p->key==plan.mod&&p->revision==revision;
                });};
                auto it=chosen();
                if(it==catalog.packs.end()){refresh();it=chosen();}
                if(it!=catalog.packs.end()){
                    const bool needs_source=(*it)->shapekey_vertices||std::any_of((*it)->components.begin(),(*it)->components.end(),[](const auto &p){return p.extra_position_input;});
                    if(needs_source)plan.disposition=WWMI_RT_AUTHOR_LEGACY;
                    const bool candidate=needs_source?(deformation_owner&&planned_shared_author_pack(*it)):owned_author_pack(*it);
                    if(!candidate)plan.freeze_cause=AuthorFreezeCause::pack_not_author;
                    else if(!(*it)->surface_supported)plan.freeze_cause=AuthorFreezeCause::no_surface;
                    else if((*it)->components.size()!=match.identity->components.size())plan.freeze_cause=AuthorFreezeCause::component_set;
                    else if(!std::all_of(match.identity->components.begin(),match.identity->components.end(),[&](const auto &part){
                        const auto &sample=part.sample;
                        return identities.live(match.identity,sample.component)&&inputs.position_submission_count(sample)==match.submissions;
                    }))plan.freeze_cause=AuthorFreezeCause::native_cohort;
                    else{
                        plan.freeze_cause=needs_source?AuthorFreezeCause::external_final:AuthorFreezeCause::rest_candidate;
                        if(deformation_owner){
                            // Keep the candidate. The actual VM must
                            // positively select a source before assembly.
                            plan.pack=*it;plan.sources.resize(plan.pack->components.size());
                        }else{
                            auto &h=author_history_for(match.identity,*it,match.producer);
                            if(!h.owned_pose){
                                auto pose=inputs.assemble_snapshot(*match.identity,*it);
                                if(pose.result==PoseResult::complete)h.owned_pose=std::move(pose.pose);
                            }
                            if(h.owned_pose){plan.pack=*it;plan.pose=h.owned_pose;}
                            else plan.freeze_cause=AuthorFreezeCause::pose_assembly;
                        }
                    }
                }
            }
            if(!plan.pack&&plan.disposition!=WWMI_RT_AUTHOR_LEGACY)plan.disposition=WWMI_RT_AUTHOR_NATIVE;
            plan.choice_checked=true;
            plan.initial_disposition=plan.disposition;
            plan.freeze_ticket=request.draw_ticket;plan.freeze_recording=captured->snapshot.source.recording;
            plan.freeze_component=match.component;
            note_author_choice(plan,plan.freeze_ticket,plan.freeze_recording,plan.freeze_component,true,WWMI_RT_AUTHOR_TENTATIVE,nullptr);
        }
        out.plan=plan.id;out.disposition=plan.disposition;
        if(!plan.pack)return true;
        if(deformation_owner)*deformation_owner=deformed_owner(plan,match.component);
        const auto &source=match.identity->components[match.component].sample;
        const auto &window=source.profile->components[source.component];
        const auto offset=12ull*window.first,bytes=12ull*window.count;
        PreviousReadUse read{};
        ResourceKey view;
        if(request.previous_resource&&offset<=request.previous_bytes&&bytes<=request.previous_bytes-offset&&
           !inputs.native_position_srv_error(request.previous_resource,request.previous_offset,request.previous_bytes,
               DXGI_FORMAT_R32_FLOAT,0,view)){
            const auto actual=inputs.position_receipt(view,request.previous_offset+offset,bytes);
            if(actual.order){
                read={view,request.previous_offset+offset,bytes,actual};
                out.previous={view.creation,actual.order,actual.producer.list,actual.producer.creation,actual.producer.generation};
            }
        }
        const auto &roots=request.previous_scope;
        const auto &component=plan.pack->components[match.component];
        const bool use_previous=(roots.primitive_words[12]&1u)|roots.view_words[19];
        const bool provider=use_previous&&std::any_of(plan.pack->components.begin(),plan.pack->components.end(),[&](const auto &part){
            return std::any_of(part.dependencies.begin(),part.dependencies.end(),[&](const auto &d){return d.provider==match.component;});
        });
        PreviousDrawScope scope{};
        bool scoped=false;
        if(use_previous&&(provider||!component.dependencies.empty())&&
           inputs.previous_cb_keys(roots.view_resource,roots.view_byte_origin,roots.primitive_resource,roots.primitive_byte_origin,
               scope.view,scope.primitive)&&scope.primitive==plan.primitive&&roots.primitive_byte_origin==plan.scope.byte_origin){
            scope.native=roots;
            auto* list=reinterpret_cast<api::command_list *>(request.native_draw.list);
            if(auto recording=hold_draw_recording(inputs.recording(list),list)){
                scoped=true;
                if(provider)note_previous_provider(*recording,scope,match.identity,plan.pack,match.component,read);
            }
        }
        // Record demand at the actual original SRV read, even when the VM or
        // an unready surface prevents private drawing later in this section.
        if(view&&component.index_count&&component.vertex_count&&component.rest_skinning_known&&!component.bone_map.empty())
            remember_previous(plan.pack,view,request.previous_offset,request.previous_bytes,match.component,
                source.content,source.content_offset,scoped,true);
        return true;
    }
    template<class Record>
    std::shared_ptr<AuthorCommands> close_author_commands(Record &&record){
        auto found=std::find_if(author_commands.begin(),author_commands.end(),[](const auto &c){return c.use_count()==1&&!c->producing;});
        std::shared_ptr<AuthorCommands> command;
        if(found!=author_commands.end())command=*found;
        else{
            command=std::make_shared<AuthorCommands>();
            if(FAILED(native->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&command->allocator)))||
               FAILED(native->CreateCommandList1(0,D3D12_COMMAND_LIST_TYPE_DIRECT,D3D12_COMMAND_LIST_FLAG_NONE,IID_PPV_ARGS(&command->list))))return {};
            author_commands.push_back(command);
        }
        if(FAILED(command->allocator->Reset())||FAILED(command->list->Reset(command->allocator.Get(),nullptr))){
            std::erase(author_commands,command);return {};
        }
        command->timer.reset();
        const bool recorded=record(command->list.Get());
        const auto close=command->list->Close();
        if(FAILED(close)||!recorded){std::erase(author_commands,command);return {};}
        return command;
    }
    std::shared_ptr<AuthorCommands> closed_author_commands(GeometryBatch &batch){
        std::shared_ptr<GpuWorkSample> timer;
        auto commands=close_author_commands([&](auto *list){
            timer=begin_work_timer(GpuWorkKind::prefix,list,batch.gpu_recording);
            const bool recorded=batch.record_owned(list);
            if(timer){
                work_placements(*timer,work_storage(reinterpret_cast<std::uint64_t>(&batch)));
                timer->work=author_prefix_vertices(batch);
                end_work_timer(*timer,list);
                if(recorded)resolve_work_timer(*timer,list);
                else timer->error="prefix_not_recorded";
            }
            return recorded;
        });
        if(commands)commands->timer=std::move(timer);
        else if(timer)timer->error="prefix_not_closed";
        return commands;
    }
    std::shared_ptr<AuthorStaticAssets> prepare_author_static_assets(const std::shared_ptr<const ModPack> &pack,bool raster_only=false){
        if(const auto existing=author_static_assets.find(pack.get());existing!=author_static_assets.end())
            return existing->second;
        rt_cpu::Scope measured(rt_cpu::assets);
        auto generation=std::make_shared<AuthorStaticAssets>();
        generation->bootstrap=GeometryAssets::create(native.Get(),pack,skin_dxil,copy_dxil,tangent_dxil,raster_only);
        try{
            generation->geometry=make_default_author_assets(*generation->bootstrap);
            gpu::check(native->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&generation->ready)),"RT author static upload fence");
            generation->upload=close_author_commands([&](auto *list){
                record_author_asset_upload(list,*generation->geometry,*generation->bootstrap);return true;
            });
            if(!generation->upload)throw std::runtime_error("RT author static upload recording");
        }catch(const std::exception &e){
            // DEFAULT placement is an optimization. No recording has been
            // given these resources; keep the already valid original assets.
            generation->geometry.reset();generation->ready.Reset();
            reshade::log::message(reshade::log::level::warning,e.what());
        }
        author_static_assets.emplace(pack.get(),generation);
        return generation;
    }
    // The first real committed consumer supplies the queue but still reads
    // the bootstrap. No committed command list depends on pending DEFAULT.
    void start_author_upload(const std::shared_ptr<AuthorStaticAssets> &assets,ID3D12CommandQueue *queue){
        if(!assets||!assets->upload||assets->producer)return;
        auto &a=*assets;
        try{uploading_author_assets.push_back(assets);}catch(const std::bad_alloc&){
            // No transfer was issued. Do not interrupt the valid bootstrap
            // prefix merely because the optimization could not own a flight.
            a.upload.reset();a.ready.Reset();a.geometry.reset();return;
        }
        a.producer=queue;
        ID3D12CommandList *upload=a.upload->list.Get();
        queue->ExecuteCommandLists(1,&upload);
        a.signalled=SUCCEEDED(queue->Signal(a.ready.Get(),1));
        if(!a.signalled)reshade::log::message(reshade::log::level::error,"RT author static upload ready Signal failed; keep bootstrap");
    }
    void collect_author_uploads(){
        std::erase_if(uploading_author_assets,[&](const auto &asset){
            auto &a=*asset;
            if(a.signalled){
                const auto done=a.ready->GetCompletedValue();
                if(done==UINT64_MAX||done<1)return false;
            }else{
                if(!writer_signal_known(a.recovery,true))return false;
            }
            // Existing recordings and flights still own their exact original
            // assets through the GeometryBatch. New bindings now use DEFAULT.
            a.bootstrap.reset();a.upload.reset();a.producer.Reset();a.ready.Reset();
            rt_cpu::hit(rt_cpu::author_default_publish);
            return true;
        });
    }
    void report_previous_palette(bool stopping=false){
        const auto phase=rt_cpu::phase.load(std::memory_order_acquire);
        if(previous_palette_reported||!phase||(!stopping&&phase<2))return;
        previous_palette_reported=true;
        char line[170];
        std::snprintf(line,sizeof(line),"RT-OWNED-PREV-PALETTE window=cpu prepared_prefixes=%llu reused_uploads=%llu",
            (unsigned long long)previous_palette_prepared,(unsigned long long)previous_palette_reused);
        reshade::log::message(reshade::log::level::info,line);
    }
    bool author_production_ready(const AuthorOutput &output){
        const auto &ready=output.production;
        if(!ready.value)return false;
        const auto q=queues.find(ready.queue);
        if(q==queues.end()||reinterpret_cast<std::uint64_t>(q->second.author_ready.Get())!=ready.fence)return false;
        auto &queue=q->second;
        if(queue.author_completed<ready.value)collect_author_producers(queue,ready.value);
        return queue.author_completed!=UINT64_MAX&&queue.author_completed>=ready.value;
    }
    bool collect_author_producers(Queue &q,std::uint64_t needed=0){
        if(!q.producer_head)return false;
        if(q.author_completed<(std::max)(q.producer_head->ready_value,needed)){
            rt_cpu::Scope poll(rt_cpu::fence_poll);
            q.author_completed=q.author_ready->GetCompletedValue();
        }
        if(q.author_completed==UINT64_MAX)return false;
        bool retired=false;
        while(q.producer_head&&q.producer_head->ready_value<=q.author_completed){
            auto *c=q.producer_head;
            q.producer_head=c->next;c->next=nullptr;c->ready_value=0;
            c->producing.reset();retired=true;
            if(auto *bin=author_work.current())++bin->command_retired;
        }
        if(!q.producer_head)q.producer_tail=nullptr;
        return retired;
    }
    static std::uint64_t author_prefix_vertices(const GeometryBatch &batch)noexcept{
        const auto &parts=batch.assets->pack->components;
        if(batch.single_component!=UINT32_MAX){
            const auto &part=parts[batch.single_component];return part.index_count?part.vertex_count:0;
        }
        std::uint64_t total=0;
        for(const auto &part:parts)if(part.index_count)total+=part.vertex_count;
        return total;
    }
    std::shared_ptr<GeometryBatch> ready_author_batch(const std::shared_ptr<AuthorOutputs> &history,
        const std::shared_ptr<const Pose> &pose,unsigned component){
        if(!history||history->pose!=pose)return {};
        auto ready=[&](const std::shared_ptr<AuthorOutput> &value)->std::shared_ptr<GeometryBatch>{
            return value&&value->pose==pose&&author_production_ready(*value)?value->batch:nullptr;
        };
        if(auto full=ready(history->full))return full;
        return component<history->parts.size()?ready(history->parts[component]):nullptr;
    }
    void execute_author_outputs(DrawBindingRecording &binding,ID3D12CommandQueue *native_queue){
        const auto native_id=reinterpret_cast<std::uint64_t>(native_queue);
        Queue *queue=nullptr;
        // A ready fence is for sharing with future recordings. Failure to
        // acquire it cannot stop an already committed consumer on this queue.
        try{
            auto &q=queues[native_id];
            if(!q.author_ready&&SUCCEEDED(native->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&q.author_ready))))
                q.native=native_queue;
            if(q.author_ready)queue=&q;
        }catch(const std::bad_alloc&){}
        // This is the real first consumer queue, before the host's native
        // Execute. No other recording can see this generation yet.
        native_queue->ExecuteCommandLists(UINT(binding.author_pending_lists.size()),binding.author_pending_lists.data());
        binding.author_pending_lists.clear();
        NativeIdentityFence ready{};
        if(queue){
            const auto next=queue->author_value+1;
            if(SUCCEEDED(native_queue->Signal(queue->author_ready.Get(),next))){
                queue->author_value=next;
                ready={native_id,reinterpret_cast<std::uint64_t>(queue->author_ready.Get()),next};
            }else reshade::log::message(reshade::log::level::error,"RT author output ready Signal failed; do not share this generation");
        }
        for(auto &p:binding.author_prefixes)if(p.committed&&!p.executed){
            p.executed=true;
            p.output->submitted=true;
            if(auto *bin=author_work.current()){
                const auto &batch=*p.output->batch;
                ++(batch.single_component==UINT32_MAX?bin->prefix_submit_full:bin->prefix_submit_component);
                bin->prefix_vertices+=author_prefix_vertices(batch);
            }
            if(const auto &timer=p.commands->timer){
                timer->pre=1;
                if(ready.value)set_work_signal(*timer,*queue,ready,true);
                else timer->error="producer_signal_missing";
            }
            rt_cpu::hit(p.output->batch->assets==p.static_assets->geometry?
                rt_cpu::author_default_submit:rt_cpu::author_bootstrap_submit);
            if(!ready.value)continue;
            p.output->production=ready;
            auto *commands=p.commands.get();
            commands->producing=p.output->batch;
            commands->ready_value=ready.value;
            if(queue->producer_tail)queue->producer_tail->next=commands;
            else queue->producer_head=commands;
            queue->producer_tail=commands;
            p.commands.reset();
            if(auto history=p.history.lock();history&&history->pose==p.output->pose){
                auto &slot=history->slot(p.output->batch->single_component);
                // Keep the first real producer for this exact coverage.
                // A duplicate never displaces an already shareable output.
                if(!slot)slot=p.output;
            }
        }
    }
    std::shared_ptr<GeometryBatch> prepare_author_streams(std::shared_ptr<const Pose> pose,Recording consumer,api::command_list *list,
        unsigned component,std::shared_ptr<AuthorOutputs> &history,const std::shared_ptr<const NativeBlasIdentity> &identity={},
        ComPtr<ID3D12Resource> *owned_previous_palette=nullptr,std::span<const SkinSample> writers={},bool raster_only=false){
        const auto binding=hold_draw_recording(consumer,list);
        if(!binding||!pose)return {};
        auto *native_list=reinterpret_cast<ID3D12GraphicsCommandList *>(list->get_native());
        if(!native_list||native_list->GetType()!=D3D12_COMMAND_LIST_TYPE_DIRECT)return {};
        if(history&&history->pose!=pose)history.reset();
        for(const auto &p:binding->author_prefixes)
            if(p.output->pose==pose&&
               (p.output->batch->single_component==UINT32_MAX||p.output->batch->single_component==component)){
                rt_cpu::hit(rt_cpu::author_local_acquire);
                if(auto *bin=author_work.current())++bin->local_acquire;
                return p.output->batch;
            }
        if(auto ready=ready_author_batch(history,pose,component)){
            auto &holds=records[consumer.list];
            if(std::none_of(holds.begin(),holds.end(),[&](const auto &v){return v.get()==ready.get();})){
                holds.push_back(ready);binding->author_batches.push_back(ready.get());
            }
            if(ready->gpu_recording!=consumer){
                rt_cpu::hit(rt_cpu::author_cross_acquire);
                if(auto *bin=author_work.current())++bin->cross_ready;
            }
            return ready;
        }
        if(!completed_author_inputs_valid(*pose,component,native.Get()))return {};
        auto static_assets=prepare_author_static_assets(pose->pack,raster_only);
        if(!static_assets)return {};
        const auto &assets=static_assets->bootstrap?static_assets->bootstrap:static_assets->geometry;
        bool shape=false;
        for(unsigned c=0;c<pose->pack->components.size();++c)
            if(pose->pack->components[c].index_count&&(component==UINT32_MAX||component==c)&&
               pose->final_positions[c].mode==position_author_shape_delta_float6)shape=true;
        if(shape){
            if(shape_dxil.empty())shape_dxil=read_asset(L"ShapePositions.cso");
            const bool vectors=component==UINT32_MAX&&bool(assets->vectors);
            if(vectors&&shape_tangent_dxil.empty())shape_tangent_dxil=read_asset(L"ShapeTangents.cso");
            // DEFAULT is a separate GeometryAssets object: initialize the
            // pipeline on the exact object this batch is about to retain.
            assets->enable_shape_positions(shape_dxil,vectors?std::span<const unsigned char>(shape_tangent_dxil):std::span<const unsigned char>{});
        }
        // The previous SRV is addressed by this component's author indices;
        // keep the full VertexID address space, but don't skin other
        // components or generate tangents for that historical-only SRV.
        const bool reused_upload=owned_previous_palette&&owned_previous_palette->Get();
        rt_cpu::Scope allocation_measured(rt_cpu::batch_create);
        bool reused_storage=false;
        auto batch=prepare_pooled_author_streams(assets,*pose,component,owned_previous_palette,author_stream_pool,reused_storage,
            ++next_author_output,!static_assets->bootstrap);
        allocation_measured.end();
        batch->gpu_recording=consumer;
        rt_cpu::Scope recording_measured(rt_cpu::skin_record);auto command=closed_author_commands(*batch);recording_measured.end();
        if(!command)return {};
        // A closed command list is not a promise to submit. Commit names the
        // subset the actual nonempty private draw is about to consume.
        if(!history){
            history=std::make_shared<AuthorOutputs>(pose->pack->components.size());
            history->pose=pose;
        }
        auto output=std::make_shared<AuthorOutput>();
        output->pose=std::move(pose);output->batch=batch;
        for(const auto &writer:writers)if(writer.recording!=output->pose->recording)
            output->other_writers.emplace_back(writer.order,writer.recording);
        records[consumer.list].push_back(batch);
        binding->author_batches.push_back(batch.get());
        binding->author_prefixes.push_back({identity,output,history,std::move(command),std::move(static_assets)});
        if(auto *bin=author_work.current())note_work_output(output,bin,reused_storage);
        if(owned_previous_palette&&rt_cpu::phase.load(std::memory_order_relaxed)==1&&
           rt_cpu::now()<rt_cpu::deadline.load(std::memory_order_acquire)){
            ++previous_palette_prepared;
            previous_palette_reused+=reused_upload;
        }
        return batch;
    }
    std::shared_ptr<GeometryBatch> prepare_author_batch(SurfaceHistory &h,Recording consumer,api::command_list *list,
        unsigned component=UINT32_MAX){
        return prepare_author_streams(h.owned_pose,consumer,list,component,h.author_outputs,h.identity);
    }
    void seal_author_acquisition(const WWMI_RtAuthorFrameRequest_v1 &q,const WWMI_RtAuthorFrame_v1 &result,unsigned status,
        unsigned issued=1,AuthorSectionLease lease={}){
        const auto &request=q.draw.base;
        const auto &draw=request.inputs.current.planned.draw;
        auto *captured=captured_author(draw.native_draw,q.draw_ticket);
        if(!captured)return;
        auto prepared=std::exchange(captured->prepared,{});
        const auto &frame=result.frame;
        const auto current=frame.current.geometry.batch_token,previous=result.previous_batch;
        // The proven HISTORY+5 path hands main a real current surface. Main
        // may commit it only after its actual shader permits a no-history draw
        // and all of the private material bindings have been prepared.
        const bool can_continue=status==WWMI_RT_SURFACE_PREVIOUS_MISSING&&
            frame.failed_phase==WWMI_RT_DRAW_HISTORY&&q.subdraw_count==1&&!previous&&
            request.flags==(WWMI_RT_DRAW_MATERIAL|WWMI_RT_DRAW_PREVIOUS);
        if(result.plan!=q.plan||!captured->source||captured->source->plan!=q.plan||frame.geometry_status!=WWMI_RT_GEOMETRY_OK||
           (status!=WWMI_RT_SURFACE_OK&&!can_continue)||
           (q.subdraw_count?!current:(current||previous))||
           (status==WWMI_RT_SURFACE_OK&&q.subdraw_count&&bool(previous)!=bool(request.flags&WWMI_RT_DRAW_PREVIOUS)))return;
        if(!q.subdraw_index)prepared={q.plan,draw.section_token,current,previous,draw.component,q.subdraw_count,0};
        if(prepared.plan!=q.plan||prepared.section!=draw.section_token||prepared.component!=draw.component||
           prepared.count!=q.subdraw_count||prepared.issued!=q.subdraw_index||
           prepared.current!=current||prepared.previous!=previous)return;
        prepared.issued+=issued;
        prepared.lease=std::move(lease);
        if(prepared.complete()&&current){
            auto binding=draw_recording(draw.native_draw.list);
            // This is the current ticket's recording (captured_author above).
            // Reserve only the actual closed lists still needed by the draw.
            const auto added=std::count_if(binding->author_prefixes.begin(),binding->author_prefixes.end(),[&](const auto &p){
                const auto token=reinterpret_cast<std::uint64_t>(p.output->batch.get());
                return !p.committed&&(token==current||token==previous);
            });
            if(added){
                binding->author_pending_batches.reserve(binding->author_pending_batches.size()+added);
                binding->author_pending_lists.reserve(binding->author_pending_lists.size()+added);
            }
        }
        captured->prepared=std::move(prepared);
    }
    bool commit_author(const WWMI_RtAuthorCommit_v1 &q,const RtAuthorCommitNote* note=nullptr){
        auto *p=find_author_plan(q.plan);if(!p)return false;auto &plan=*p;
        if(inputs.recording(reinterpret_cast<api::command_list *>(q.list))!=Recording{q.list,q.creation,q.recording})return false;
        auto binding=draw_recording(q.list);
        if(!binding||binding->epoch!=Recording{q.list,q.creation,q.recording}||!q.draw_ticket||
            binding->author_capture.snapshot.draw_ticket!=q.draw_ticket||binding->author_capture.snapshot.token!=plan.snapshot)return false;
        const auto reset=binding->reset.load(std::memory_order_acquire);
        if(reset==DrawBindingRecording::waiting||reset==DrawBindingRecording::succeeded)return false;
        if(q.disposition==WWMI_RT_AUTHOR_NATIVE){
            if(plan.disposition==WWMI_RT_AUTHOR_PRIVATE){
                plan.refused_native=true;
                note_author_choice(plan,q.draw_ticket,q.recording,q.component,false,plan.disposition,note,true);
                return false;
            }
            if(plan.disposition==WWMI_RT_AUTHOR_LEGACY){plan.completed_source_closed=true;return true;}
            const auto from=plan.disposition;
            plan.disposition=WWMI_RT_AUTHOR_NATIVE;
            if(from!=plan.disposition)note_author_choice(plan,q.draw_ticket,q.recording,q.component,false,from,note);
            return true;
        }
        if(q.disposition!=WWMI_RT_AUTHOR_PRIVATE||plan.disposition==WWMI_RT_AUTHOR_NATIVE||!plan.pack||
           (!plan.pose&&(!plan.requires_source||!q.empty_selection))||
           (plan.requires_source&&(!binding->author_capture.source||binding->author_capture.source->plan!=plan.id)))return false;
        if(q.component>=plan.pack->components.size()||bool(q.empty_selection)!=plan.pack->components[q.component].draws.empty())return false;
        const auto holds=records.find(q.list);
        const auto held=[&](std::uint64_t token){
            return token&&holds!=records.end()&&std::any_of(holds->second.begin(),holds->second.end(),[&](const auto &h){
                return reinterpret_cast<std::uint64_t>(h.get())==token;
            });
        };
        if(q.empty_selection?(q.current_batch||q.previous_batch):(!held(q.current_batch)||(q.previous_batch&&!held(q.previous_batch))))return false;
        if(plan.requires_source){
            const auto &prepared=binding->author_capture.prepared;
            if(q.component>=plan.sources.size()||!prepared.complete()||prepared.plan!=plan.id||prepared.component!=q.component||
               bool(q.empty_selection)!=!prepared.count||prepared.current!=q.current_batch||prepared.previous!=q.previous_batch)return false;
        }else if(!q.empty_selection){
            const auto *h=history_for(plan.identity,plan.pack);
            const auto completed_batch=h?ready_author_batch(h->author_outputs,h->owned_pose,UINT32_MAX):nullptr;
            const bool expected=(completed_batch&&reinterpret_cast<std::uint64_t>(completed_batch.get())==q.current_batch)||
                std::any_of(binding->author_prefixes.begin(),binding->author_prefixes.end(),[&](const auto &p){
                    const auto &batch=p.output->batch;
                    return p.identity==plan.identity&&batch->assets->pack==plan.pack&&batch->single_component==UINT32_MAX&&
                        reinterpret_cast<std::uint64_t>(batch.get())==q.current_batch;
                });
            if(!expected)return false;
        }
        // V2 already reserved these entries when sealing the actual complete
        // section. Preserve the older acquisition/commit contract for v1.
        if(!plan.requires_source){
            binding->author_pending_batches.reserve(binding->author_pending_batches.size()+2);
            binding->author_pending_lists.reserve(binding->author_pending_lists.size()+2);
        }
        for(auto &p:binding->author_prefixes){
            const auto token=reinterpret_cast<std::uint64_t>(p.output->batch.get());
            if(!p.committed&&(token==q.current_batch||token==q.previous_batch)){
                p.committed=true;
                binding->author_pending_batches.push_back(p.output->batch.get());
                binding->author_pending_lists.push_back(p.commands->list.Get());
            }
        }
        if(plan.requires_source){
            auto &capture=binding->author_capture;
            plan.sources[q.component]={capture.prepared.section,*capture.source,q.empty_selection!=0};
            capture.prepared={};
            history_owners_changed();
        }
        if(!q.empty_selection)++binding->committed_sections;
        const auto from=plan.disposition;
        plan.disposition=WWMI_RT_AUTHOR_PRIVATE;
        if(from!=plan.disposition)note_author_choice(plan,q.draw_ticket,q.recording,q.component,false,from,note);
        return true;
    }
