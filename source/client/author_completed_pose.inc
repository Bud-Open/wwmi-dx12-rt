    // Included inside Runtime, alongside author_draw.inc. Every nonempty
    // component is acquired with its own exact owner; a full resource range
    // alone never grants ownership of another component.
    std::shared_ptr<const Pose> acquire_completed_author_pose(const AuthorPlan &plan,
        const NativeDrawIdentityCache::Match &match,Recording consumer,const WWMI_RtDeformedSelection_v1 &selected){
        if(!plan.pack||!plan.identity||plan.identity!=match.identity||
           (selected.kind!=WWMI_RT_DEFORMED_AUTHOR_FLOAT6&&selected.kind!=WWMI_RT_DEFORMED_FINAL_PAIR))return {};
        const auto &pack=*plan.pack;const auto &identity=*plan.identity;
        if(identity.components.size()!=pack.components.size()||match.component>=pack.components.size())return {};
        auto expected=deformed_owner(plan,match.component);
        expected.kind=selected.kind;expected.recipe_receipt=selected.recipe_receipt;
        if(std::memcmp(&expected,&selected,sizeof(expected)))return {};
        if(plan.pose){
            if(match.component>=plan.pose->final_positions.size())return {};
            const auto &held=plan.pose->final_positions[match.component].completed_author;
            return held&&!std::memcmp(&held->source().selection,&selected,sizeof(selected))?plan.pose:nullptr;
        }
        std::vector<FinalPosition> sources(pack.components.size());
        for(unsigned c=0;c<pack.components.size();++c){
            const auto &part=pack.components[c];if(!part.index_count)continue;
            const auto &sample=identity.components[c].sample;
            if(!identities.live(plan.identity,c)||inputs.position_submission_count(sample)!=match.submissions)return {};
            auto owner=deformed_owner(plan,c);owner.kind=selected.kind;owner.recipe_receipt=selected.recipe_receipt;
            auto source=CompletedAuthorInput::acquire(mod_acquire_deformed,native.Get(),owner,consumer);
            if(!source||!source->covers(part.vertex_first,part.vertex_count)||
               !CompletedAuthorInput::bind(source,sources[c]))return {};
        }
        // AuthorPlan is frozen once selected. A changed publication never
        // mutates a Pose already referenced by a draw or previous cohort.
        PoseInputs values;values.reset(identity.recording);
        BlasPoseInput original{identity.recording,{}};
        for(const auto &part:identity.components){
            if(!values.add(part.sample))return {};
            original.geometries.push_back(part.vertices);
        }
        auto assembled=values.assemble(original,plan.pack,std::move(sources));
        if(assembled.result!=PoseResult::complete||!assembled.pose)return {};
        if(!completed_author_inputs_valid(*assembled.pose,UINT32_MAX,native.Get()))return {};
        return std::shared_ptr<const Pose>(std::move(assembled.pose));
    }
