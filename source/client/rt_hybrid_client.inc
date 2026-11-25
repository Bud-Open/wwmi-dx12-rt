// Inside the 06 Runtime, under its existing device mutex.
    std::map<std::tuple<std::uint64_t,std::uint64_t,std::uint64_t>,std::shared_ptr<HybridAsset>> hybrid_assets;
    std::vector<HybridPose> hybrid_poses;
    std::uint64_t hybrid_serial=0,hybrid_load=0;
    std::uint64_t hybrid_probe_load=0,hybrid_probe_native_rev=0;
    std::map<std::uint64_t,std::uint64_t> hybrid_probe_checked;

    bool hybrid_probe(const WWMI_RtPackDrawQuery_v2 &q,std::uint64_t ticket,std::uint64_t load,std::uint64_t mod){
        const auto *cap=captured_author(q,ticket);
        if(!cap||!load||!mod)return false;
        const auto &id=cap->match.identity;
        // Positive ownership of the existing route takes priority. This check
        // does not run the VM or alter the author selection / target cache.
        const auto target=targets.find(id->native_blas);
        if(target!=targets.end()&&target->second->native_identity==id){
            const auto &pack=target->second->geometry->assets->pack;
            if(pack->load_generation==load&&pack->key==mod)return false;
        }
        if(std::any_of(author_plans.begin(),author_plans.end(),[&](const auto &p){
            return p->identity==id&&p->load==load&&p->mod==mod&&p->disposition==WWMI_RT_AUTHOR_PRIVATE;
        }))return false;
        // If this Mod has a complete evaluated selection and can be owned by
        // the private RT author path (rest_skinning_known + surface_supported),
        // keep it on the private RT route so it retains full RT reflections.
        // Otherwise (e.g. shapekey_vertices > 0, disjoint merged VG windows
        // with !rest_skinning_known, local/remapped skeleton, or build_pack
        // rejection), route cleanly to Hybrid Raster.
        if(mod_author_revision){
            const auto revision=mod_author_revision(load,mod);
            if(revision){
                auto find_pack=[&](){
                    return std::find_if(catalog.packs.begin(),catalog.packs.end(),[&](const auto &p){
                        return p->load_generation==load&&p->key==mod&&p->revision==revision;
                    });
                };
                auto it=find_pack();
                if(it==catalog.packs.end()){
                    const auto native_rev=native_profiles?native_profiles->snapshot()->revision:0ull;
                    if(hybrid_probe_load!=load||hybrid_probe_native_rev!=native_rev){
                        hybrid_probe_checked.clear();
                        hybrid_probe_load=load;
                        hybrid_probe_native_rev=native_rev;
                    }
                    if(hybrid_probe_checked[mod]!=revision){
                        hybrid_probe_checked[mod]=revision;
                        refresh();
                        it=find_pack();
                    }
                }
                if(it!=catalog.packs.end()&&owned_author_pack(*it)&&(*it)->surface_supported&&
                   (*it)->components.size()==id->components.size())
                    return false;
            }
        }
        return true;
    }
    void hybrid_previous(const WWMI_RtPackDrawQuery_v2 &q,std::uint64_t ticket,const wwmi::hybrid::HistoryRead &previous){
        auto *cap=captured_author(q,ticket);
        if(!cap)return;
        auto &f=cap->raster_previous;f={};
        const auto &match=cap->match;
        const auto &s=match.identity->components[match.component].sample;
        const auto offset=12ull*s.profile->components[s.component].first;
        if(!previous.resource||offset>previous.bytes||s.output_bytes>previous.bytes-offset||
           previous.offset>UINT64_MAX-offset||
           inputs.native_position_srv_error(previous.resource,previous.offset,previous.bytes,DXGI_FORMAT_R32_FLOAT,0,f.resource))
            return;
        f.offset=previous.offset+offset;f.bytes=s.output_bytes;f.view_offset=previous.offset;
        f.receipt=inputs.position_receipt(f.resource,f.offset,f.bytes);
    }
    std::shared_ptr<HybridAsset> hybrid_asset(const wwmi::hybrid::Asset &a,const std::shared_ptr<const NativeProfile> &profile){
        namespace h=wwmi::hybrid;
        if(hybrid_load!=a.load){
            for(const auto &[_,old]:hybrid_assets)author_static_assets.erase(old->pack.get());
            hybrid_assets.clear();hybrid_poses.clear();hybrid_load=a.load;
        }
        const auto key=std::make_tuple(a.load,a.mod,a.version);
        if(auto it=hybrid_assets.find(key);it!=hybrid_assets.end())
            return it->second->pack->native==profile?it->second:nullptr;
        if(!profile||profile->components.size()!=a.bone_windows)return {};
        auto asset=std::make_shared<HybridAsset>();
        if(!asset->proof.load(a))return {};
        auto pack=std::make_shared<ModPack>();
        pack->load_generation=a.load;pack->key=a.mod;pack->revision=a.version;pack->native=profile;
        pack->vertices=a.vertices;pack->shapekey_vertices=a.shapes;pack->canonical_bones=asset->proof.bones;
        pack->components.resize(profile->components.size());
        std::vector<bool> seen_rows(pack->canonical_bones);
        for(unsigned i=0;i<pack->components.size();++i){
            auto &c=pack->components[i];c.native=profile->components[i];
            const auto *w=std::find_if(a.bones,a.bones+a.bone_windows,[&](const auto &b){return b.component==c.native.component;});
            if(w==a.bones+a.bone_windows)return {};
            for(unsigned j=0;j<w->count;++j){
                if(seen_rows[w->first+j])return {}; // no unproved overwrite of a canonical row
                seen_rows[w->first+j]=true;c.bone_map.push_back({j,w->first+j});
            }
        }
        if(!std::all_of(seen_rows.begin(),seen_rows.end(),[](bool v){return v;}))return {};
        // No triangle topology belongs to the skin batch. The immutable
        // real IB is held independently, retaining the signed IA base.
        auto set=[&](auto &out,unsigned stream){const auto in=h::view(a.streams[stream]);out.assign(in.begin(),in.end());};
        set(pack->positions,h::positions);pack->blends=asset->proof.normalized;
        const auto un=a.streams[h::uv].size;const auto uv_stride=a.streams[h::uv].stride;
        if(a.streams[h::vectors].size==8ull*a.vertices)set(pack->vectors,h::vectors);
        if(un&&un%(4ull*a.vertices)==0&&un/(4ull*a.vertices)<=8&&uv_stride==4*un/(4ull*a.vertices)){
            set(pack->uv,h::uv);pack->uv_pairs=unsigned(un/(4ull*a.vertices));
        }
        if(a.streams[h::color].size==4ull*a.vertices)set(pack->color,h::color);
        pack->surface_supported=!pack->vectors.empty()&&pack->uv_pairs;
        const auto ib=h::view(a.streams[h::indices]);
        if(ib.size()>UINT32_MAX)return {};
        asset->index=gpu::buffer(native.Get(),ib.size(),D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);
        gpu::upload(asset->index.Get(),ib.data(),ib.size());
        asset->pack=std::move(pack);
        hybrid_assets.emplace(key,asset);
        return asset;
    }
    std::shared_ptr<const Pose> assemble_hybrid_pose(const NativeBlasIdentity &id,
        const std::shared_ptr<const ModPack> &pack){
        PoseInputs original;original.reset(id.recording);BlasPoseInput layout{id.recording,{}};
        for(const auto &c:id.components){if(!original.add(c.sample))return {};layout.geometries.push_back(c.vertices);}
        auto result=original.assemble_raster(layout,pack);
        return result.result==PoseResult::complete?std::move(result.pose):nullptr;
    }
    HybridPose *hybrid_previous_pose(const AuthorCapture::RasterPrevious &read,
        const std::shared_ptr<HybridAsset> &asset,const std::shared_ptr<const NativeBlasIdentity> &id,
        unsigned component,ResourceKey primitive,std::uint64_t origin){
        if(!read.resource||!read.receipt.order||read.receipt.submissions!=1||!read.receipt.native_queue)return nullptr;
        const auto receipt=inputs.position_receipt(read.resource,read.offset,read.bytes);
        if(receipt.producer!=read.receipt.producer||receipt.order!=read.receipt.order||
           receipt.submissions!=1||receipt.native_queue!=read.receipt.native_queue)return nullptr;
        const auto &now=id->components[component];
        const auto matches=[&](const auto &source){
            if(!source||source==id||source->recording!=receipt.producer||
               source->components.size()!=id->components.size())return false;
            const auto &old=source->components[component];
            const auto &s=old.sample;
            return s.output==read.resource&&s.order==receipt.order&&s.recording==receipt.producer&&
                s.profile==now.sample.profile&&s.component==now.sample.component&&
                s.content==now.sample.content&&s.content_offset==now.sample.content_offset&&
                old.indices==now.indices&&old.index_offset==now.index_offset&&old.index_count==now.index_count&&
                old.vertices.vertex==read.resource&&old.vertices.offset==read.view_offset&&
                s.output_offset==read.offset&&s.output_bytes==read.bytes&&inputs.position_live(s);
        };
        const auto proved=[&](NativeIdentityFence signal){
            return signal.queue==receipt.native_queue&&writer_signal_known(signal,false);
        };
        // Cached and native-history entries are two ways to prove the
        // previous identity, not competing lookup routes. Repeated views of
        // the same identity are fine; a different linked cohort is not.
        std::shared_ptr<const NativeBlasIdentity> exact;
        const auto consider=[&](const std::shared_ptr<const NativeBlasIdentity> &identity){
            if(exact)return exact==identity;
            exact=identity;return true;
        };
        HybridPose *found=nullptr;
        for(auto &old:hybrid_poses){
            if(old.asset!=asset||!matches(old.identity))continue;
            const bool adopted=old.primitive==primitive&&old.origin==origin&&
                old.adopted[component]!=wwmi::hybrid::unavailable;
            if(!adopted&&!proved(old.linked_history))continue;
            if(!consider(old.identity))return nullptr;
            if(!found||adopted)found=&old;
        }
        // The VS's original typed SRV identifies a written position range.
        // Its submitted, linked native snapshot proves all palette providers
        // even if there was no previously adopted draw at this upload origin.
        // The immutable author mesh uses those same local-space bone rows;
        // the draw's original VS still supplies its own previous transform.
        NativeIdentityFence signal{};
        for(const auto &old:surface_history){
            if(old.pack||!proved(old.native_signal)||!matches(old.identity))continue;
            if(!consider(old.identity))return nullptr;
            signal=old.native_signal;
        }
        if(!exact)return nullptr;
        if(found){
            if(signal.queue)found->linked_history=signal;
            return found;
        }
        for(auto &old:hybrid_poses)if(old.asset==asset&&old.identity==exact){
            old.linked_history=signal;return &old;
        }
        auto pose=assemble_hybrid_pose(*exact,asset->pack);
        if(!pose)return nullptr;
        return &hybrid_poses.emplace_back(HybridPose{asset,std::move(exact),{},0,std::move(pose),{},
            std::vector<wwmi::hybrid::Result>(id->components.size(),wwmi::hybrid::unavailable),signal});
    }
    wwmi::hybrid::Result acquire_hybrid(const wwmi::hybrid::Request &q,wwmi::hybrid::Frame &out){
        namespace h=wwmi::hybrid;
        if(!q.asset||!q.asset->version||q.count>4096||(q.count&&!q.parts)||
           !hybrid_probe(q.native,q.ticket,q.asset->load,q.asset->mod))return h::unavailable;
        auto *cap=captured_author(q.native,q.ticket);
        const auto match=cap->match;
        const auto &id=match.identity;
        auto *list=reinterpret_cast<api::command_list *>(q.native.list);
        const auto consumer=inputs.recording(list);
        auto binding=hold_draw_recording(consumer,list);
        if(!binding||match.component>=id->components.size())return h::unavailable;
        const auto primitive=inputs.primitive_key(q.instance.resource,q.instance.byte_origin);
        if(!primitive)return h::unavailable;
        if(!q.count)return h::empty_selection;
        const auto &sample=id->components[match.component].sample;
        auto asset=hybrid_asset(*q.asset,sample.profile);
        if(!asset)return h::unavailable;
        bool shape=false,remap_mismatch=false;
        for(unsigned i=0;i<q.count;++i){
            const auto window=asset->proof.prove(q.parts[i]);
            if(!window.valid)return h::unavailable;
            shape|=window.shape;
            remap_mismatch|=window.remap_mismatch;
        }
        // In base-skin hybrid mode (allow_base), GPU skinning evaluates every
        // vertex directly against the unified canonical merged skeleton
        // (pack->blends, proven vg < bones), without relying on DX11's
        // per-component 256-bone BlendRemapForward/Reverse table. Allowing
        // valid-canonical cross-component subdraws prevents whole-component
        // fallbacks (hollow meshes) when a mod moves a submesh across
        // components to share another component's material/shader pass.
        if((shape||remap_mismatch)&&!q.allow_base)return h::unavailable;
        const auto mode=(shape||remap_mismatch)?h::base_skin:h::exact;
        std::erase_if(hybrid_poses,[&](const auto &p){
            return ((!p.primitive||!inputs.resource_current(p.primitive))&&!writer_signal_known(p.linked_history,false))||
                std::none_of(p.identity->components.begin(),p.identity->components.end(),[&](const auto &c){
                    return inputs.position_live(c.sample);
                });
        });
        // This precise native multi-component BLAS is already linked by 06.
        // Do not merge disjoint VG windows from a second raw dispatch/ring.
        NativeIdentityFence live_producer=match.producer;
        unsigned live_submissions=match.submissions;
        const bool current_live=identities.live_state(id,live_submissions,live_producer)&&
            live_submissions<=1&&(match.submissions==0||live_submissions==1)&&
            std::all_of(id->components.begin(),id->components.end(),[&](const auto &c){
                return c.sample.recording==id->recording&&identities.live(id,c.sample.component)&&
                    inputs.position_submission_count(c.sample)==live_submissions;
            });
        if(!current_live)return h::unavailable;
        cap->match.submissions=live_submissions;
        cap->match.producer=live_producer;
        auto same_scope=[&](const auto &p){return p.asset==asset&&p.primitive==primitive&&p.origin==q.instance.byte_origin;};
        auto cached=std::find_if(hybrid_poses.begin(),hybrid_poses.end(),[&](const auto &p){return same_scope(p)&&p.identity==id;});
        if(cached==hybrid_poses.end()){
            auto pose=assemble_hybrid_pose(*id,asset->pack);
            if(!pose)return h::unavailable;
            HybridPose value{asset,id,primitive,q.instance.byte_origin,std::move(pose),{},
                std::vector<h::Result>(id->components.size(),h::unavailable)};
            cached=hybrid_poses.insert(hybrid_poses.end(),std::move(value));
        }
        auto &current=*cached;
        const auto already=current.adopted[match.component];
        if(already!=h::unavailable&&already!=mode){
            // In basic mode, no-Shape subdraws share the very same bytes; only
            // the dependency/accuracy classification differs between passes.
            if(!q.allow_base)return h::unavailable;
        }
        const auto &pack=*asset->pack;
        if(q.need_material&&(!pack.surface_supported||(q.need_color&&pack.color.empty())))return h::unavailable;
        // Recovering the linked previous snapshot may grow hybrid_poses.
        const auto current_pose=current.pose;
        auto batch=prepare_author_streams(current.pose,consumer,list,UINT32_MAX,current.outputs,id,nullptr,{},true);
        if(!batch)return h::unavailable;
        auto &surface=out.current;
        surface.struct_size=sizeof(surface);surface.abi_version=1;surface.vertex_count=pack.vertices;surface.uv_pairs=pack.uv_pairs;
        auto &g=surface.geometry;
        g.struct_size=sizeof(g);g.abi_version=1;
        g.consumer_list=consumer.list;g.consumer_list_creation=consumer.creation;g.consumer_recording=consumer.generation;
        g.batch_token=reinterpret_cast<std::uint64_t>(batch.get());
        g.load_generation=pack.load_generation;g.mod_key=pack.key;g.mod_revision=pack.revision;
        g.position_resource=reinterpret_cast<std::uint64_t>(batch->positions.Get());
        g.position_gpu_va=batch->positions->GetGPUVirtualAddress();g.position_bytes=12ull*pack.vertices;g.position_stride=12;
        g.position_format=DXGI_FORMAT_R32G32B32_FLOAT;g.position_state=position_read_state;
        g.index_resource=reinterpret_cast<std::uint64_t>(asset->index.Get());
        g.index_gpu_va=asset->index->GetGPUVirtualAddress();g.index_bytes=asset->proof.source_indices.size();g.index_format=DXGI_FORMAT_R32_UINT;
        if(q.need_material){
            if(!batch->tangents||!batch->tangent_ready[0])return h::unavailable;
            surface.uv=surface_view(batch->assets->uv.Get(),pack.uv.size(),DXGI_FORMAT_R16G16_FLOAT,4,4*pack.uv_pairs,D3D12_RESOURCE_STATE_GENERIC_READ);
            surface.tangents=surface_view(batch->tangents.Get(),32ull*pack.vertices,DXGI_FORMAT_R32G32B32A32_FLOAT,16,32,position_read_state);
            surface.color=surface_view(batch->assets->color.Get(),pack.color.size(),DXGI_FORMAT_R8G8B8A8_UNORM,4,4,D3D12_RESOURCE_STATE_GENERIC_READ);
        }
        std::shared_ptr<GeometryBatch> previous;
        if(q.need_previous){
            if(auto *which=hybrid_previous_pose(cap->raster_previous,asset,id,match.component,primitive,q.instance.byte_origin))
                previous=prepare_author_streams(which->pose,consumer,list,UINT32_MAX,which->outputs,which->identity,nullptr,{},true);
            if(previous)out.previous.positions=surface_view(previous->positions.Get(),12ull*pack.vertices,DXGI_FORMAT_R32_FLOAT,4,12,position_read_state);
        }
        auto &holds=records[consumer.list];
        if(std::none_of(holds.begin(),holds.end(),[&](const auto &x){return x.get()==asset.get();}))holds.push_back(asset);
        // The recording owns the exact immutable signed-base IB.
        auto &pending=binding->hybrid_pending;
        pending={++hybrid_serial,q.ticket,match.component,mode,current_pose,std::move(batch),std::move(previous)};
        out.transaction=pending.token;out.mode=mode;
        return q.need_previous&&!pending.previous?h::previous_missing:mode;
    }
    bool commit_hybrid(std::uint64_t list,std::uint64_t ticket,std::uint64_t token){
        auto binding=draw_recording(list);
        if(!binding||binding->epoch!=inputs.recording(reinterpret_cast<api::command_list *>(list))||
           binding->author_capture.snapshot.draw_ticket!=ticket)return false;
        auto &r=binding->hybrid_pending;
        if(!token||r.token!=token||r.ticket!=ticket)return false;
        auto chosen=std::find_if(hybrid_poses.begin(),hybrid_poses.end(),[&](const auto &p){return p.pose==r.pose;});
        if(chosen==hybrid_poses.end()||!r.current)return false;
        // Same retention and pre-Execute ordering as the independent author
        // outputs, without changing AuthorPlan, Target, BLAS or any RT source.
        const auto need=[&](const auto &p){return !p.committed&&(p.output->batch==r.current||p.output->batch==r.previous);};
        const auto count=std::count_if(binding->author_prefixes.begin(),binding->author_prefixes.end(),need);
        binding->author_pending_batches.reserve(binding->author_pending_batches.size()+count);
        binding->author_pending_lists.reserve(binding->author_pending_lists.size()+count);
        for(auto &p:binding->author_prefixes)if(need(p)){
            binding->author_pending_batches.push_back(p.output->batch.get());
            binding->author_pending_lists.push_back(p.commands->list.Get());p.committed=true;
        }
        chosen->adopted[r.component]=r.mode;
        r={};++binding->committed_sections;
        return true;
    }
