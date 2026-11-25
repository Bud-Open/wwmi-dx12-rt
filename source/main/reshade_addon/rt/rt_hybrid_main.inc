// Within the same TU as the ordinary 06 private-section transaction.
namespace hybrid = wwmi::hybrid;
namespace hybrid_origin = wwmi::author_position_origin;
struct RtHybridAsset {
    hybrid::Asset abi;
    std::vector<hybrid::BoneWindow> bones;
    struct Input {
        const WwmiMod::Resource* decl = nullptr;
        PortApi::BufferResourceInfo bind;
        Microsoft::WRL::ComPtr<ID3D12Resource> hold;
        std::vector<unsigned char> bytes;
    };
    std::array<Input,hybrid::stream_count> streams;
    const WwmiMod::Resource* merged_skeleton = nullptr;
    bool local_skeleton = false;
    bool mod_ib_r16 = false;
    std::vector<std::int16_t> vertex_comp;
    std::vector<unsigned char> canonical_blends;
    mutable std::map<std::tuple<unsigned,unsigned,int,int>,bool> local_windows;
    struct RemapPair {
        const WwmiMod::Resource* blend = nullptr;
        const WwmiMod::Resource* skeleton = nullptr;
    };
    std::unordered_map<int,RemapPair> remapped_comps;
};
extern bool WwmiIsHybridBaseSkinMode() noexcept;
static bool RtHybridBaseAllowed(){
    return WwmiIsHybridBaseSkinMode();
}
static bool RtHybridFormat(const WwmiMod::Resource& resource,const wchar_t* format){
    wchar_t value[64]{};
    if(!PortApi::GetSectionValue(resource.fq.c_str(),L"format",value,std::size(value)))return false;
    return _wcsicmp(value,format)==0||
        (_wcsnicmp(format,L"DXGI_FORMAT_",12)==0&&_wcsicmp(value,format+12)==0);
}
static bool RtHybridBufferDecl(const WwmiMod::Resource* res,PortApi::BufferResourceInfo& info){
    return res&&PortApi::GetBufferResource(res->fq.c_str(),&info)&&info.dx12&&info.gpu_va&&info.byte_width;
}
static bool RtHybridBuffer(const WwmiMod::Mod& mod,const wchar_t* name,PortApi::BufferResourceInfo& info){
    return RtHybridBufferDecl(WwmiMod::Find(mod,name),info);
}
struct RtLocalDrawWindow {
    unsigned count = 0;
    unsigned start = 0;
    int base = 0;
};
struct RtLocalDrawScan {
    std::wstring ns_fq;
    std::vector<RtLocalDrawWindow>* out = nullptr;
    std::set<std::wstring>* visited = nullptr;
    int depth = 0;
};
static void RtLocalDrawScanKey(const wchar_t* key,const wchar_t* val,void* user){
    auto *sc=static_cast<RtLocalDrawScan*>(user);
    if(!sc||!key||!val||sc->depth>4)return;
    if(_wcsicmp(key,L"drawindexed")==0){
        unsigned cnt=0,start=0;int base=0;
        if(swscanf_s(val,L"%u , %u , %d",&cnt,&start,&base)==3&&cnt>0){
            const bool dup=std::any_of(sc->out->begin(),sc->out->end(),[&](const auto &d){
                return d.count==cnt&&d.start==start&&d.base==base;
            });
            if(!dup)sc->out->push_back(RtLocalDrawWindow{cnt,start,base});
        }
        return;
    }
    if(_wcsicmp(key,L"run")==0&&sc->depth<4){
        std::wstring t(val);
        while(!t.empty()&&(t.front()==L' '||t.front()==L'\t'))t.erase(t.begin());
        while(!t.empty()&&(t.back()==L' '||t.back()==L'\t'))t.pop_back();
        std::wstring fq;
        if(_wcsnicmp(t.c_str(),L"CommandList",11)==0&&t.size()>11&&t[11]!=L'\\')
            fq=L"CommandList\\"+sc->ns_fq+L"\\"+t.substr(11);
        else if(_wcsnicmp(t.c_str(),L"CustomShader",12)==0&&t.size()>12&&t[12]!=L'\\')
            fq=L"CustomShader\\"+sc->ns_fq+L"\\"+t.substr(12);
        if(!fq.empty()&&sc->visited->insert(fq).second){
            RtLocalDrawScan sub{sc->ns_fq,sc->out,sc->visited,sc->depth+1};
            PortApi::ForEachSectionKey(fq.c_str(),&RtLocalDrawScanKey,&sub);
        }
    }
}
static std::vector<RtLocalDrawWindow> RtCollectCompDraws(const std::wstring& ns,const WwmiMod::Component& c){
    std::vector<RtLocalDrawWindow> out;
    out.reserve(c.draws.size());
    for(const auto &d:c.draws)out.push_back({d.count,d.start,0});
    if(ns.empty()||c.tail.empty())return out;
    wchar_t fq[512]{};
    if(!PortApi::ResolveSection(ns.c_str(),nullptr,c.tail.c_str(),fq,std::size(fq))||!fq[0])return out;
    const std::wstring fq_str(fq);
    const auto first_sep=fq_str.find(L'\\');
    const auto last_sep=fq_str.rfind(L'\\');
    if(first_sep==std::wstring::npos||last_sep==std::wstring::npos||last_sep<=first_sep)return out;
    const std::wstring ns_fq=fq_str.substr(first_sep+1,last_sep-first_sep-1);
    std::set<std::wstring> visited{fq_str};
    RtLocalDrawScan sc{ns_fq,&out,&visited,0};
    PortApi::ForEachSectionKey(fq_str.c_str(),&RtLocalDrawScanKey,&sc);
    return out;
}
static std::shared_ptr<const RtHybridAsset> RtLoadHybridAsset(int mod){
    // Only this lane looks at these streams. EngineScope is held by the VM.
    static std::map<int,std::shared_ptr<const RtHybridAsset>> cache;
    static std::set<int> static_rejected;
    static unsigned last_load=0;
    static std::uint64_t version=0;
    const auto load=PortApi::LoadGeneration();
    if(load!=last_load){cache.clear();static_rejected.clear();last_load=load;}
    if(static_rejected.count(mod))return {};
    const auto &m=g_geomods[mod].mod;
    static const wchar_t* const names[]{
        L"ResourcePositionBuffer",L"ResourceBlendBuffer",L"ResourceIndexBuffer",L"ResourceVectorBuffer",
        L"ResourceTexCoordBuffer",L"ResourceColorBuffer",L"ResourceShapeKeyVertexIdBuffer",
        L"ResourceBlendRemapVertexVGBuffer",L"ResourceBlendRemapForwardBuffer",L"ResourceBlendRemapReverseBuffer"
    };
    static const wchar_t* const formats[]{
        L"DXGI_FORMAT_R32G32B32_FLOAT",nullptr,L"DXGI_FORMAT_R32_UINT",L"DXGI_FORMAT_R8G8B8A8_SNORM",
        L"DXGI_FORMAT_R16G16_FLOAT",L"DXGI_FORMAT_R8G8B8A8_UNORM",L"DXGI_FORMAT_R32_UINT",
        L"DXGI_FORMAT_R16_UINT",L"DXGI_FORMAT_R16_UINT",L"DXGI_FORMAT_R16_UINT"
    };
    const auto valid=[&](const auto &inputs){
        for(unsigned i=0;i<hybrid::stream_count;++i){
            const auto &v=inputs[i];
            if(!v.hold)continue;
            PortApi::BufferResourceInfo live{};
            if(!RtHybridBufferDecl(v.decl,live)||live.dx12!=v.bind.dx12||live.gpu_va!=v.bind.gpu_va||
               live.byte_width!=v.bind.byte_width||live.stride!=v.bind.stride||
               !hybrid_origin::still_initial(v.hold.Get(),live.byte_width))return false;
        }
        return true;
    };
    if(const auto i=cache.find(mod);i!=cache.end()&&valid(i->second->streams))return i->second;
    const auto reject_static=[&]()->std::shared_ptr<const RtHybridAsset>{
        static_rejected.insert(mod);
        return {};
    };
    auto a=std::make_shared<RtHybridAsset>();
    a->abi.load=load;a->abi.mod=std::uint64_t(mod)+1;a->abi.version=++version;a->abi.vertices=m.mesh_vertex_count;
    const auto shapes=PortApi::GetVarNs(g_geomods[mod].wcl_ns.c_str(),L"shapekey_vertex_count",0.f);
    if(!std::isfinite(shapes)||shapes<0.f||double(shapes)>double(UINT32_MAX)/4||
       float(unsigned(shapes))!=shapes)return {};
    a->abi.shapes=unsigned(shapes);
    a->merged_skeleton=WwmiMod::Find(m,L"ResourceMergedSkeleton");
    for(unsigned i=0;i<hybrid::stream_count;++i){
        if(i==hybrid::shape_ids&&a->abi.shapes==0)continue;
        const auto *decl=WwmiMod::Find(m,names[i]);auto &in=a->streams[i];
        if(i==hybrid::shape_ids&&!decl&&RtHybridBaseAllowed()){
            a->abi.shapes=0;
            continue;
        }
        const bool required=i<=hybrid::indices||(i==hybrid::shape_ids&&a->abi.shapes>0);
        if(!decl){if(required)return reject_static();continue;}
        in.decl=decl;
        if(i==hybrid::indices){
            if(RtHybridFormat(*decl,L"DXGI_FORMAT_R16_UINT"))a->mod_ib_r16=true;
            else if(!RtHybridFormat(*decl,L"DXGI_FORMAT_R32_UINT"))return reject_static();
        }else if(formats[i]&&!RtHybridFormat(*decl,formats[i]))return reject_static();
        if(!RtHybridBufferDecl(decl,in.bind))return {};
        in.hold=in.bind.dx12;
        if(!hybrid_origin::still_initial(in.hold.Get(),in.bind.byte_width))return {};
        in.bytes.resize(in.bind.byte_width);
        if(!hybrid_origin::read_initial(in.hold.Get(),in.bytes.data(),in.bind.byte_width))return {};
        if(i==hybrid::indices&&a->mod_ib_r16){
            if(in.bytes.empty()||(in.bytes.size()&1))return reject_static();
            const std::size_t cnt=in.bytes.size()/2;
            std::vector<unsigned char> widened(cnt*4);
            for(std::size_t k=0;k<cnt;++k){
                std::uint16_t v16=0;std::memcpy(&v16,in.bytes.data()+2*k,2);
                const std::uint32_t v32=v16;std::memcpy(widened.data()+4*k,&v32,4);
            }
            in.bytes=std::move(widened);
        }
        if(i==hybrid::shape_ids){
            if(in.bytes.size()%4!=0)return reject_static();
            const unsigned actual_shapes=unsigned(in.bytes.size()/4);
            if(actual_shapes!=a->abi.shapes){
                logger::write("WwmiHybrid: mod=%d guid=%u reconciled shapekey_vertex_count %u -> %u from ResourceShapeKeyVertexIdBuffer",
                    mod+1,m.object_guid,a->abi.shapes,actual_shapes);
                a->abi.shapes=actual_shapes;
            }
            if(actual_shapes==0){
                in={};
                a->abi.streams[i]={nullptr,0,0};
                continue;
            }
        }
        unsigned eff_stride=i==hybrid::indices?4u:in.bind.stride;
        if(i==hybrid::blends&&!eff_stride&&a->abi.vertices&&in.bytes.size()%a->abi.vertices==0)
            eff_stride=unsigned(in.bytes.size()/a->abi.vertices);
        // WWMI files can declare stride=12 on an R32_UINT *index* buffer.
        // The native IA uses the index format, as does this proof.
        a->abi.streams[i]={in.bytes.data(),in.bytes.size(),eff_stride};
    }
    if(a->merged_skeleton){
        for(const auto &c:m.comps){
            if(c.index<0)return reject_static();
            a->bones.push_back({unsigned(c.index),c.vg_offset,c.vg_count});
            if(c.remapped){
                const auto suffix=std::to_wstring(c.index);
                a->remapped_comps[c.index]={
                    WwmiMod::Find(m,(L"ResourceRemappedBlendBufferComponent"+suffix).c_str()),
                    WwmiMod::Find(m,(L"ResourceRemappedSkeletonComponent"+suffix).c_str())
                };
            }
        }
    }else{
        // Universal Per-Component Native Skeleton WWMI format (merge_vg = False):
        // Each component leaves vs-cb4 on its native component bone palette and
        // indexes local bones (0 .. max_local_bone). Synthesize disjoint canonical
        // bone windows and widen blend rows to stride-32 canonical indices.
        if(m.comps.empty()||!a->abi.vertices||a->abi.vertices>65535u*64u||
           a->streams[hybrid::full_vg].decl||a->streams[hybrid::forward].decl||a->streams[hybrid::reverse].decl)
            return reject_static();
        const auto &ib_bytes=a->streams[hybrid::indices].bytes;
        const auto &blend_in=a->streams[hybrid::blends];
        const unsigned bstride=a->abi.streams[hybrid::blends].stride;
        if(ib_bytes.empty()||ib_bytes.size()%4||(bstride!=8&&bstride!=16&&bstride!=32)||
           blend_in.bytes.size()!=std::uint64_t(bstride)*a->abi.vertices)return reject_static();
        const unsigned bslots=bstride==8?4u:8u;
        const std::size_t n_idx=ib_bytes.size()/4;
        a->local_skeleton=true;
        a->vertex_comp.assign(a->abi.vertices,std::int16_t(-1));
        std::unordered_map<int,unsigned> max_bone;
        const auto &ns=g_geomods[mod].wcl_ns;
        for(const auto &c:m.comps){
            if(c.index<0||c.index>32767||c.remapped||c.vg_count)return reject_static();
            max_bone[c.index]=0u;
            const auto draws=RtCollectCompDraws(ns,c);
            for(const auto &d:draws){
                if(d.start>n_idx||d.count>n_idx-d.start)return reject_static();
                for(unsigned k=0;k<d.count;++k){
                    std::uint32_t raw_v=0;
                    std::memcpy(&raw_v,ib_bytes.data()+4ull*(std::size_t(d.start)+k),4);
                    const std::int64_t vid=std::int64_t(raw_v)+d.base;
                    if(vid<0||vid>=std::int64_t(a->abi.vertices))return reject_static();
                    const auto v=std::uint32_t(vid);
                    bool weighted=false;
                    unsigned v_max=0;
                    for(unsigned s=0;s<bslots;++s){
                        std::uint16_t lb=0,w16=0;
                        if(bstride==8){
                            lb=blend_in.bytes[8ull*v+s];
                            w16=std::uint16_t(blend_in.bytes[8ull*v+4+s])*257u;
                        }else if(bstride==16){
                            lb=blend_in.bytes[16ull*v+s];
                            w16=std::uint16_t(blend_in.bytes[16ull*v+8+s])*257u;
                        }else{
                            std::memcpy(&lb,blend_in.bytes.data()+32ull*v+2*s,2);
                            std::memcpy(&w16,blend_in.bytes.data()+32ull*v+16+2*s,2);
                        }
                        if(w16){
                            weighted=true;
                            if(lb>=512u)return reject_static();
                            if(lb>v_max)v_max=lb;
                        }
                    }
                    if(weighted){
                        auto &own=a->vertex_comp[v];
                        if(own==-1)own=std::int16_t(c.index);
                        else if(own!=std::int16_t(c.index))return reject_static();
                        if(v_max>max_bone[c.index])max_bone[c.index]=v_max;
                    }
                }
            }
        }
        unsigned running_first=0;
        std::unordered_map<int,unsigned> comp_first;
        for(const auto &c:m.comps){
            const unsigned cnt=max_bone[c.index]+1u;
            if(cnt>512u||running_first>65536u-cnt)return reject_static();
            comp_first[c.index]=running_first;
            a->bones.push_back({unsigned(c.index),running_first,cnt});
            running_first+=cnt;
        }
        a->canonical_blends.resize(32ull*a->abi.vertices,0);
        for(std::uint32_t v=0;v<a->abi.vertices;++v){
            const std::int16_t own=a->vertex_comp[v];
            const unsigned base_b=own>=0?comp_first[own]:0u;
            for(unsigned s=0;s<bslots;++s){
                std::uint16_t lb=0,w16=0;
                if(bstride==8){
                    lb=blend_in.bytes[8ull*v+s];
                    w16=std::uint16_t(blend_in.bytes[8ull*v+4+s])*257u;
                }else if(bstride==16){
                    lb=blend_in.bytes[16ull*v+s];
                    w16=std::uint16_t(blend_in.bytes[16ull*v+8+s])*257u;
                }else{
                    std::memcpy(&lb,blend_in.bytes.data()+32ull*v+2*s,2);
                    std::memcpy(&w16,blend_in.bytes.data()+32ull*v+16+2*s,2);
                }
                const std::uint16_t cb=(w16&&own>=0)?std::uint16_t(base_b+lb):std::uint16_t(0);
                std::memcpy(a->canonical_blends.data()+32ull*v+2*s,&cb,2);
                std::memcpy(a->canonical_blends.data()+32ull*v+16+2*s,&w16,2);
            }
        }
        a->abi.streams[hybrid::blends]={a->canonical_blends.data(),a->canonical_blends.size(),32u};
    }
    a->abi.bones=a->bones.data();a->abi.bone_windows=unsigned(a->bones.size());
    cache[mod]=a;return a;
}
static bool RtHybridVb(unsigned slot,const PortApi::BufferResourceInfo& want){
    void* resource=nullptr;unsigned long long va=0;unsigned bytes=0,stride=0;
    return want.dx12&&PortApi::GetShimVertexBuffer(slot,&resource,&va,&bytes,&stride,nullptr)&&
        resource==want.dx12&&va==want.gpu_va&&bytes>=want.byte_width&&stride==want.stride&&want.stride;
}
static bool RtHybridVb0(const MigotoPort::PipeState& state,const PortApi::BufferResourceInfo& want,
    unsigned load,int mod,unsigned guid){
    if(RtHybridVb(0,want))return true;
    if(!RtHybridBaseAllowed()||!want.dx12||!want.byte_width||want.stride!=12)return false;
    void* resource=nullptr;unsigned long long va=0;unsigned bytes=0,stride=0;
    if(!PortApi::GetShimVertexBuffer(0,&resource,&va,&bytes,&stride,nullptr))return false;
    const auto *vb0=MigotoPort::AsWrapResource(state.vb[0]);
    // Base-skin hybrid mode: accept any mod-owned (non-foreign) stride-12 position
    // buffer covering the mod's vertex stream (e.g., custom [Present] ShapeKey CS
    // output copied via `cs-uN = copy ...; Resource* = ref cs-uN`, or alternate
    // variant position buffers). Never accept foreign native VB0.
    if(!vb0||vb0->foreign||!vb0->dx12||resource!=vb0->dx12||!va||stride!=want.stride||bytes<want.byte_width)
        return false;
    static std::mutex s_log_mtx;
    static unsigned s_log_gen=~0u;
    static std::set<int> s_logged;
    {
        std::lock_guard lk(s_log_mtx);
        if(s_log_gen!=load){s_logged.clear();s_log_gen=load;}
        if(s_logged.insert(mod).second){
            logger::probe("WWMI-LOAD phase=base-skin-vb0-accept generation=%u mod=%d guid=%u vb0=%p bytes=%u stride=%u base_bytes=%u",
                load,mod+1,guid,resource,bytes,stride,want.byte_width);
        }
    }
    return true;
}
static bool RtHybridCb(unsigned slot,const PortApi::BufferResourceInfo& want){
    const auto &state=MigotoPort::State();
    const auto *bound=MigotoPort::AsWrapResource(state.cb[MigotoPort::StageIndex('v')][slot]);
    return want.dx12&&bound&&bound->dx12==want.dx12&&bound->dx12->GetGPUVirtualAddress()+bound->gpu_va==want.gpu_va;
}
static bool RtFreezeHybridPart(StreamContext& ctx,SectionDraw& sd,StreamDraw& draw){
    const auto fail=[&](const char* why){
        if(!ctx.rejection){ctx.rejection=why;ctx.rejected_rfx=draw.rfx;}
        return false;
    };
    auto &a=ctx.hybrid_asset;
    if(!a)a=RtLoadHybridAsset(ctx.mod);
    if(!a||!sd.wc)return fail("freeze-asset");
    const auto &s=a->streams;
    for(const auto &in:s)if(in.hold&&!hybrid_origin::still_initial(in.hold.Get(),in.bind.byte_width))return fail("freeze-mutated");
    const auto &state=MigotoPort::State();
    const auto *ib=MigotoPort::AsWrapResource(state.ib);
    const DXGI_FORMAT expected_ib_fmt=a->mod_ib_r16?DXGI_FORMAT_R16_UINT:DXGI_FORMAT_R32_UINT;
    if(!ib||ib->dx12!=s[hybrid::indices].bind.dx12||state.ib_offset||
       (state.ib_format!=expected_ib_fmt&&state.ib_format!=DXGI_FORMAT_UNKNOWN)||
       ib->dx12->GetGPUVirtualAddress()+ib->gpu_va!=s[hybrid::indices].bind.gpu_va||
       !RtHybridVb0(state,s[hybrid::positions].bind,a->abi.load,ctx.mod,g_geomods[ctx.mod].mod.object_guid))
        return fail("freeze-ib-vb0");
    if(ctx.program){
        if(!RtHybridVb(1,s[hybrid::vectors].bind)||!RtHybridVb(2,s[hybrid::uv].bind)||
           (ctx.program->aux>=0&&!ctx.surface().aux_constant&&!RtHybridVb(3,s[hybrid::color].bind)))
            return fail("freeze-mat-vb");
    }
    // Resource identity must match both members of the evaluated pair.
    auto &p=draw.hybrid;p={draw.count,draw.first,draw.base};
    if(a->local_skeleton){
        // RtPublishWorldSkeletonInputs pre-populates state.cb['v'][4] with a
        // foreign WrapBuffer pointing at ctx.palette.{resource,gpu_va}. Accept
        // either unbound or that untouched native palette wrapper; reject any
        // custom non-foreign buffer substitution.
        const auto *cb4=MigotoPort::AsWrapResource(state.cb[MigotoPort::StageIndex('v')][4]);
        const bool native_cb4=!cb4||(cb4->foreign&&
            cb4->dx12==reinterpret_cast<ID3D12Resource*>(ctx.palette.resource)&&
            cb4->gpu_va==ctx.palette.gpu_va);
        if(!RtHybridVb(4,s[hybrid::blends].bind)||!native_cb4)return fail("freeze-local-cb4");
        const std::tuple<unsigned,unsigned,int,int> wkey{draw.count,draw.first,draw.base,sd.wc->index};
        if(const auto it=a->local_windows.find(wkey);it!=a->local_windows.end())
            return it->second?true:fail("freeze-local-win");
        const auto &ib_bytes=s[hybrid::indices].bytes;
        const std::size_t n_idx=ib_bytes.size()/4;
        bool ok=draw.count&&draw.count%3==0&&draw.first<=n_idx&&draw.count<=n_idx-draw.first;
        for(unsigned k=0;ok&&k<draw.count;++k){
            std::uint32_t raw_i=0;
            std::memcpy(&raw_i,ib_bytes.data()+4ull*(std::size_t(draw.first)+k),4);
            const std::int64_t id=std::int64_t(raw_i)+draw.base;
            if(id<0||id>=std::int64_t(a->vertex_comp.size())){ok=false;break;}
            const std::int16_t own=a->vertex_comp[std::size_t(id)];
            if(own!=-1&&own!=std::int16_t(sd.wc->index)){ok=false;break;}
        }
        a->local_windows.emplace(wkey,ok);
        return ok?true:fail("freeze-local-win");
    }
    PortApi::BufferResourceInfo palette{};
    if(RtHybridVb(4,s[hybrid::blends].bind)&&RtHybridBufferDecl(a->merged_skeleton,palette)&&RtHybridCb(4,palette))
        return true;
    if(!sd.wc->remapped||sd.wc->remap_id<0||!sd.wc->remap_vg)return fail("freeze-merged-cb4");
    const auto ri=a->remapped_comps.find(sd.wc->index);
    if(ri==a->remapped_comps.end())return fail("freeze-remap-comp");
    PortApi::BufferResourceInfo remapped{};
    if(!RtHybridBufferDecl(ri->second.blend,remapped)||!RtHybridVb(4,remapped)||
       !RtHybridBufferDecl(ri->second.skeleton,palette)||!RtHybridCb(4,palette))return fail("freeze-remap-cb4");
    p.blend=hybrid::paired_remap;p.remap_row=unsigned(sd.wc->remap_id);p.remap_count=sd.wc->remap_vg;
    return true;
}
static bool RtFinishHybridSection(StreamContext& ctx,bool complete,InjectFrame& fr){
    const auto reject=[&](const char* why,const StreamDraw* part=nullptr) {
        if(!ctx.rejection){
            ctx.rejection=why;
            ctx.rejected_rfx=part?part->rfx:ctx.rfx_latch;
        }
        const char* eff=ctx.rejection?ctx.rejection:why;
        if(eff&&std::strcmp(eff,"vm-not-complete")!=0&&ctx.mod>=0&&ctx.mod<int(g_geomods.size())){
            static std::mutex s_ref_mtx;
            static unsigned s_ref_gen=~0u;
            static std::set<std::pair<int,std::string>> s_ref_seen;
            const unsigned cur_gen=PortApi::LoadGeneration();
            std::lock_guard lk(s_ref_mtx);
            if(s_ref_gen!=cur_gen){s_ref_seen.clear();s_ref_gen=cur_gen;}
            if(s_ref_seen.size()<32&&s_ref_seen.emplace(ctx.mod,std::string(eff)).second){
                logger::probe("WWMI-HYBRID-REFUSE generation=%u mod=%d guid=%u comp=%d reason=%s",
                    cur_gen,ctx.mod+1,g_geomods[ctx.mod].mod.object_guid,
                    int(ctx.identity.native.component),eff);
            }
        }
        return false;
    };
    if(!complete||ctx.hybrid_failed){
        if(!complete){
            bool known=false,gfx_pso=false,refused=false;uint32_t level=0;
            const bool have_scope=RtNativeBindingRestoreInfo(fr.da.d3d12_cl,known,level,gfx_pso,refused);
            if(refused||ctx.rfx_latch.called){
                static std::mutex s_vm_mtx;
                static unsigned s_vm_gen=~0u;
                static std::set<std::tuple<int,int,int>> s_vm_seen;
                const unsigned cur_gen=PortApi::LoadGeneration();
                std::lock_guard lk(s_vm_mtx);
                if(s_vm_gen!=cur_gen){s_vm_seen.clear();s_vm_gen=cur_gen;}
                if(s_vm_seen.size()<32&&s_vm_seen.emplace(ctx.mod,int(ctx.identity.native.component),refused?1:0).second&&
                   ctx.mod>=0&&ctx.mod<int(g_geomods.size())){
                    logger::probe("WWMI-HYBRID-INCOMPLETE generation=%u mod=%d guid=%u comp=%d vs=%016llx refused=%d scope=%d known=%d level=%u gfx=%d rfx_call=%u",
                        cur_gen,ctx.mod+1,g_geomods[ctx.mod].mod.object_guid,int(ctx.identity.native.component),
                        (unsigned long long)(ctx.program?ctx.program->hash:0ull),
                        int(refused),int(have_scope),int(known),unsigned(level),int(gfx_pso),unsigned(ctx.rfx_latch.called));
                }
            }
            return reject(refused?"port-work-refused":"vm-not-complete");
        }
        return reject("queued-subdraw-refused");
    }
    // 'handling = skip', with a completed VM and no queued draw, is an
    // intentional hide; failure while freezing a draw or loading the mod's
    // hybrid asset is never that case.
    if(ctx.requested.empty()){
        if(!ctx.hybrid_asset)ctx.hybrid_asset=RtLoadHybridAsset(ctx.mod);
        if(!ctx.hybrid_asset)return reject("asset-missing");
        ++ctx.empty_selections;ctx.replaced=true;return true;
    }
    if(!ctx.hybrid_asset)return reject("asset-missing");
    for(const auto &in:ctx.hybrid_asset->streams)
        if(in.hold&&!hybrid_origin::still_initial(in.hold.Get(),in.bind.byte_width))return reject("asset-changed");
    const auto &world=ctx.surface();
    std::vector<hybrid::Part> recipe;recipe.reserve(ctx.requested.size());
    for(const auto& p:ctx.requested)recipe.push_back(p.hybrid);
    hybrid::Request req{ctx.query,ctx.author.draw_ticket,ctx.hybrid_instance,&ctx.hybrid_asset->abi,recipe.data(),unsigned(recipe.size()),
        RtHybridBaseAllowed(),ctx.program!=nullptr,ctx.program&&ctx.program->aux>=0&&!world.aux_constant,
        ctx.program&&world.reads_previous};
    hybrid::Frame frame;
    const auto result=ctx.reader.hybrid_acquire(&req,&frame);
    if(result!=hybrid::exact&&result!=hybrid::base_skin&&result!=hybrid::previous_missing)
        return reject("acquire", &ctx.requested.front());
    std::optional<RtHistoryInvalid> no_history;
    if(result==hybrid::previous_missing){
        if(!ctx.program||ctx.depth_only||!ctx.reader.reserve2)return reject("previous-unavailable", &ctx.requested.front());
        no_history.emplace();
        if(!RtPrepareHistoryInvalid(fr.da.cmd_list,*ctx.program,world,*no_history))
            return reject("previous-fallback-refused", &ctx.requested.front());
    }
    struct Material {RtMaterialTables tables;uint64_t local_cb=0;std::array<uint64_t,WWMI_RT_NH_CB_COUNT> history{};};
    std::vector<Material> unique_materials;
    std::vector<unsigned> part_mat_index(ctx.requested.size(),0u);
    if(ctx.program){
        RtWorldMaterialInputs shared_inputs;
        RtMaterialTables init_tables{};
        if(!RtPrepareWorldMaterialInputs(fr.da.cmd_list,*ctx.program,world,frame.current,frame.previous,
             no_history.has_value(),shared_inputs,init_tables,ctx.requested.size()>1)){
            auto &p0=ctx.requested.front();
            p0.rfx.material_failure=init_tables.failure;
            return reject("recorded-table-refused", &p0);
        }
        auto same_ps=[](const std::vector<ShadowSlotOvr>& a,const std::vector<ShadowSlotOvr>& b) noexcept {
            if(a.size()!=b.size())return false;
            for(size_t k=0;k<a.size();++k)
                if(a[k].param!=b[k].param||a[k].offset!=b[k].offset||a[k].src.ptr!=b[k].src.ptr)return false;
            return true;
        };
        std::vector<unsigned> unique_rep;
        unique_materials.reserve(ctx.requested.size());
        unique_rep.reserve(ctx.requested.size());
        for(unsigned i=0;i<ctx.requested.size();++i){
            auto &p=ctx.requested[i];
            unsigned found=UINT32_MAX;
            for(unsigned u=0;u<unique_rep.size();++u){
                if(same_ps(p.ps,ctx.requested[unique_rep[u]].ps)){found=u;break;}
            }
            if(found!=UINT32_MAX){
                part_mat_index[i]=found;
                p.rfx=ctx.requested[unique_rep[found]].rfx;
                continue;
            }
            const unsigned u_idx=unsigned(unique_materials.size());
            unique_materials.emplace_back();
            unique_rep.push_back(i);
            part_mat_index[i]=u_idx;
            auto &m=unique_materials.back();
            RtMaterialCopyScope copy_note(p.rfx,p.ps_owners);
            if(!RtPrepareWorldMaterialPart(fr.da.cmd_list,world,frame.current,shared_inputs,p.ps,p.textures,
                 ctx.reader.reserve,m.tables,m.local_cb,ctx.reader.reserve2,no_history?&*no_history:nullptr)){
                p.rfx.material_failure=m.tables.failure;
                p.rfx.material_param=m.tables.missing_param;p.rfx.material_offset=m.tables.missing_offset;
                return reject(p.rfx.vs_alias_conflict?"ps-vs-view-conflict":"recorded-table-refused", &p);
            }
            if(no_history)m.history=no_history->va;
        }
    }
    const auto &g=frame.current.geometry;
    if(!g.position_gpu_va||!g.index_gpu_va||g.position_bytes>UINT32_MAX||g.index_bytes>UINT32_MAX)
        return reject("views-invalid", &ctx.requested.front());
    RestoreGameState(fr);
    if(!ctx.reader.hybrid_commit(ctx.query.device,ctx.query.list,ctx.author.draw_ticket,frame.transaction))
        return reject("hybrid-commit-refused", &ctx.requested.front());
    const auto &graphics=CmdGfx(fr.da.cmd_list);
    auto *cl=fr.da.d3d12_cl;
    const auto issued_pso=ctx.depth_only?graphics.gfx_pso:graphics.gfx_restore();
    cl->SetPipelineState(reinterpret_cast<ID3D12PipelineState*>(issued_pso));
    const D3D12_VERTEX_BUFFER_VIEW vb{g.position_gpu_va,UINT(g.position_bytes),g.position_stride};
    const D3D12_INDEX_BUFFER_VIEW ib{g.index_gpu_va,UINT(g.index_bytes),DXGI_FORMAT_R32_UINT};
    cl->IASetVertexBuffers(0,1,&vb);cl->IASetIndexBuffer(&ib);
    unsigned bound_mat=UINT32_MAX;
    for(unsigned i=0;i<ctx.requested.size();++i){
        const unsigned m_idx=part_mat_index[i];
        if(ctx.program&&m_idx!=bound_mat){
            bound_mat=m_idx;
            RtBindMaterialTables(cl,unique_materials[m_idx].tables);
            cl->SetGraphicsRootConstantBufferView(world.local_param,unique_materials[m_idx].local_cb);
            if(no_history)for(unsigned j=0;j<WWMI_RT_NH_CB_COUNT;++j)
                if(unique_materials[m_idx].history[j])cl->SetGraphicsRootConstantBufferView(world.history_roots[j].param,unique_materials[m_idx].history[j]);
        }
        const auto &part=ctx.requested[i].hybrid;
        cl->DrawIndexedInstanced(part.count,1,part.first,part.base,0);
        const auto& kept=ctx.requested[i];
        RtMaterialDrawNote(fr.da.cmd_list,ctx.mod,ctx.identity.native.component,fr.da.draw_rt.rtv_count,
            issued_pso,RtNoteRoute::hybrid,frame.mode==hybrid::base_skin?"base-skin":"exact",
            part.first,part.count,part.base,&kept.rfx,kept.ps,ctx.program?&unique_materials[m_idx].tables:nullptr,frame.current.uv_pairs);
        ++ctx.recorded_subdraws;
    }
    if(ctx.program){
        if(no_history){
            for(unsigned j=0;j<WWMI_RT_NH_CB_COUNT;++j)if(no_history->va[j])
                cl->SetGraphicsRootConstantBufferView(world.history_roots[j].param,world.history_roots[j].va);
            ctx.history_invalid_draw=true;
        }
        cl->SetGraphicsRootConstantBufferView(world.local_param,world.native_cb);
        RfxRestore(cl,unique_materials.back().tables.original);
    }
    ctx.replaced=true;return true;
}
