// Inside the actual Runtime, immediately after author_draw.inc. Entry for a
// publisher of the original Build's sealed scene and shared Primitive source.
// Do not call it with a RayBuildUse and a row merely observed via Map.
bool issue_author_occlusion(api::command_list *list,const D3D12_DISPATCH_RAYS_DESC &dispatch,
    const RaySceneInput &ray,const OcclusionNativeScene &scene,const OcclusionInstanceLink &link,
    const OcclusionBindingRead &native_bindings,const OcclusionProgramUse *sealed_program=nullptr){
    if(!author_path||!occlusion_known_no_procedural(ray)||!occlusion_instance_matches(scene,link)||
       !(link.instance.InstanceMask&1u)|| // actual pinned MainRG TraceRay mask
       native_bindings.stage!=OcclusionBindingRead::ready||!native_bindings.table_layout.immutable_descriptors())return false;
    const auto epoch=inputs.recording(list);
    if(!epoch.list||epoch!=native_bindings.recording||(!native_bindings.closed_sbt&&!sealed_program))return false;
    const OcclusionProgramUse original{epoch,ray.compute.pipeline,ray.compute.root_signature};
    const auto &program=native_bindings.closed_sbt?original:*sealed_program;
    if(!program.matches(ray_scene.raygens,epoch,ray,native_bindings))return false;
    const auto submitted_write=[&](const OcclusionBufferWrite &writer){
        return writer.kind==OcclusionBufferWrite::sealed_upload||
            (writer.kind==OcclusionBufferWrite::command_snapshot&&writer_signal_known(writer.submit,true));
    };
    if(!submitted_write(scene.writer)||(!native_bindings.closed_sbt&&
       (!submitted_write(program.raygen.writer)||!submitted_write(program.miss0.writer))))return false;
    const auto original_build=ray_scene.build_use(inputs,epoch,ray);
    if(!original_build||!occlusion_build_matches(*original_build,scene.build))return false;
    // The cross-list source receipt is not the private prefix's recording.
    // Until actual queue ordering for an in-flight native scene is available,
    // only a single known completed native writer or earlier same-list Build
    // can supply the unchanged native instances to this consumer.
    if(original_build->recording==epoch?original_build->submissions!=0:
       (original_build->submissions!=1||!writer_signal_known(original_build->submit,true)))return false;
    if(!mod_author_revision||!inputs.resource_current(link.primitive.resource))return false;
    const AuthorPlan *plan=nullptr;
    for(const auto &choice:author_plans){
        if(!occlusion_primitive_matches(*choice,link)||!choice->pack||
           mod_author_revision(choice->load,choice->mod)!=choice->pack->revision)continue;
        if(plan)return false; // no arbitrarily picking one of two active plans
        plan=choice.get();
    }
    if(!plan||!occlusion_author_matches(*plan,link)||!inputs.resource_current(plan->identity->allocation)||
       inputs.resource_at(link.instance.AccelerationStructure)!=plan->identity->allocation)return false;
    // A shared skinning batch is a geometry cache, not a recipe receipt
    // for the other components or for another Primitive.
    // A private scope alone does not prove every component consumed this
    // author's generation; a late refused native request can still execute.
    if(!scene_sources_sealed(*plan))return false;
    for(unsigned component=0;component<plan->identity->components.size();++component)
        if(!identities.live(plan->identity,component))return false;
    auto *history=history_for(plan->identity,plan->pack);
    if(!history||history->owned_pose!=plan->pose)return false;
    const auto bound_raygen=inputs.ray_input_range(dispatch.RayGenerationShaderRecord.StartAddress,
        dispatch.RayGenerationShaderRecord.SizeInBytes);
    if(bound_raygen.key!=ray.raygen.key||bound_raygen.offset!=ray.raygen.offset||bound_raygen.bytes!=ray.raygen.bytes)return false;
    if(native_bindings.closed_sbt){
        const auto miss=inputs.ray_input_range(dispatch.MissShaderTable.StartAddress,dispatch.MissShaderTable.SizeInBytes);
        if(miss.key!=native_bindings.miss.key||miss.offset!=native_bindings.miss.offset||miss.bytes!=native_bindings.miss.bytes)return false;
    }
    const auto binding=hold_draw_recording(epoch,list);
    if(!binding)return false;
    auto surface=prepare_author_batch(*history,epoch,list);
    if(!surface||surface->single_component!=UINT32_MAX)return false;
    const auto prefix=std::find_if(binding->author_prefixes.begin(),binding->author_prefixes.end(),[&](const auto &p){
        return p.identity==plan->identity&&p.output->pose==plan->pose&&p.output->batch==surface;
    });
    // Other recordings may use only this exact full output once the
    // production fence (not raster consumer retirement) has completed.
    // A local unsubmitted prefix is legal only on this recording's queue.
    if(prefix==binding->author_prefixes.end()&&
       ready_author_batch(history->author_outputs,plan->pose,UINT32_MAX)!=surface)return false;
    auto native_scene=OcclusionScene::prepare(native.Get(),epoch,ray,original_build->destination,
        scene,link,plan->identity,*plan->pose,std::move(surface));
    if(!native_scene)return false;
    auto page=take_binding_page(*binding,OcclusionPass::descriptors,0);
    if(!page)return false;
    const auto offset=std::uint64_t(page->descriptors_used)*page->increment;
    page->descriptors_used+=OcclusionPass::descriptors;
    OcclusionTablePage tables{page->heap,page,{page->heap->GetCPUDescriptorHandleForHeapStart().ptr+offset},
        {page->heap->GetGPUDescriptorHandleForHeapStart().ptr+offset},OcclusionPass::descriptors};
    auto pass=OcclusionPass::prepare(native.Get(),list,epoch,ray,dispatch,ray_scene.raygens,program,native_bindings,
        std::move(native_scene),std::move(tables));
    if(!pass||!pass->can_issue(epoch))return false;

    // No further preparation/Close after this point. Replays continue to
    // hold the scene, BLAS, author position, and immutable descriptor page;
    // the existing pre/post native submit moves an additional hold to Flight.
    const bool commit_prefix=prefix!=binding->author_prefixes.end()&&!prefix->committed;
    if(commit_prefix){
        binding->author_pending_batches.reserve(binding->author_pending_batches.size()+1);
        binding->author_pending_lists.reserve(binding->author_pending_lists.size()+1);
    }
    records[epoch.list].push_back(pass);
    if(commit_prefix){
        prefix->committed=true;
        binding->author_pending_batches.push_back(prefix->output->batch.get());
        binding->author_pending_lists.push_back(prefix->commands->list.Get());
        // The submit boundary alone publishes a shareable generation after
        // executing its prefix and successfully signaling production.
    }
    return pass->issue(epoch);
}
