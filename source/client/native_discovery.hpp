#pragma once
#include "native_profile_registry.hpp"
#include "hsin_pose.hpp"
#include <span>
#include <array>
#include <cstring>
extern "C" std::uint32_t crc32c_append(std::uint32_t,const unsigned char *,std::size_t);
namespace hsin_rt {
inline std::uint32_t native_mesh_content_hash(std::span<const unsigned char> bytes){
    if(bytes.empty()||bytes.size()>UINT32_MAX)return 0;
    const std::uint32_t desc[]{std::uint32_t(bytes.size()),0,9,0,0,0};
    return crc32c_append(crc32c_append(0,bytes.data(),bytes.size()),reinterpret_cast<const unsigned char *>(desc),sizeof(desc));
}
struct NativeRange {ResourceKey resource;std::uint64_t offset=0,bytes=0,base=0;};
inline bool native_overlap(const NativeRange &a,const NativeRange &b){
    return a.resource==b.resource&&a.bytes&&b.bytes&&a.offset<=UINT64_MAX-a.bytes&&b.offset<=UINT64_MAX-b.bytes&&a.offset<b.offset+b.bytes&&b.offset<a.offset+a.bytes;
}
// Native index windows identify geometry entries; their enumeration order is
// not a component label. Accessors keep the negative D3D prefilter allocation-free.
template<class WindowAt,class GeometryAt> inline bool native_geometry_window_map(std::size_t count,WindowAt window,GeometryAt geometry,std::vector<unsigned> *mapping=nullptr,unsigned index_stride=4){
    if(!count||count>4096||(index_stride!=2&&index_stride!=4))return false;
    const auto stride=std::uint64_t(index_stride);
    std::uint64_t first=UINT64_MAX,address=UINT64_MAX;
    for(std::size_t i=0;i<count;++i){first=std::min(first,std::uint64_t(window(i).first));address=std::min(address,geometry(i).first);}
    if(address<stride*first)return false;const auto base=address-stride*first;
    if(mapping)mapping->clear();
    for(std::size_t i=0;i<count;++i){const auto w=window(i);unsigned match=UINT32_MAX;
        if(base>UINT64_MAX-stride*w.first)return false;
        for(std::size_t g=0;g<count;++g){const auto v=geometry(g);if(v.first==base+stride*w.first&&v.second==w.count){if(match!=UINT32_MAX)return false;match=unsigned(g);}}
        if(match==UINT32_MAX)return false;
        // Identical declared windows cannot manufacture two distinct entries.
        for(std::size_t j=0;j<i;++j)if(window(j).first==w.first&&window(j).count==w.count)return false;
        if(mapping)mapping->push_back(match);
    }
    return true;
}
// Subset/superset geometry window alignment for characters whose RT BLAS omits
// non-raytraced sections (e.g. translucent eye/FX layers with bVisibleInRayTracing=false)
// or bundles extra auxiliary sections. Always prefers exact 1-to-1 alignment when counts match.
template<class WindowAt,class GeometryAt> inline bool native_geometry_window_map_subset(
    std::size_t intent_count,WindowAt window,std::size_t geom_count,GeometryAt geometry,
    std::vector<unsigned> *mapping=nullptr,unsigned index_stride=4,
    std::uint64_t *ib_base_out=nullptr,std::size_t *matched_out=nullptr){
    if(!intent_count||intent_count>4096||!geom_count||geom_count>4096||(index_stride!=2&&index_stride!=4))return false;
    const auto stride=std::uint64_t(index_stride);
    // Reject intents with duplicate (first, count) declarations.
    for(std::size_t i=0;i<intent_count;++i){const auto wi=window(i);
        for(std::size_t j=0;j<i;++j){const auto wj=window(j);if(wj.first==wi.first&&wj.count==wi.count)return false;}
    }
    if(geom_count==intent_count){
        std::vector<unsigned> exact_map;
        if(native_geometry_window_map(intent_count,window,geometry,mapping?&exact_map:nullptr,index_stride)){
            if(ib_base_out){
                std::uint64_t first=UINT64_MAX,addr=UINT64_MAX;
                for(std::size_t i=0;i<intent_count;++i){first=std::min(first,std::uint64_t(window(i).first));addr=std::min(addr,geometry(i).first);}
                *ib_base_out=addr-stride*first;
            }
            if(mapping)*mapping=std::move(exact_map);
            if(matched_out)*matched_out=intent_count;
            return true;
        }
    }
    std::uint64_t total_intent_indices=0;
    for(std::size_t i=0;i<intent_count;++i)total_intent_indices+=window(i).count;
    std::vector<std::uint64_t> tried_bases;
    std::vector<unsigned> best_map;
    std::uint64_t best_base=0;
    std::size_t best_matched=0;
    bool best_valid=false,ambiguous=false;
    for(std::size_t i=0;i<intent_count;++i){
        const auto wi=window(i);
        const auto wi_off=stride*std::uint64_t(wi.first);
        for(std::size_t g=0;g<geom_count;++g){
            const auto vg=geometry(g);
            if(vg.second!=wi.count||vg.first<wi_off)continue;
            const std::uint64_t base=vg.first-wi_off;
            if(std::find(tried_bases.begin(),tried_bases.end(),base)!=tried_bases.end())continue;
            tried_bases.push_back(base);
            std::vector<unsigned> cand_map(intent_count,UINT32_MAX);
            std::vector<bool> used_geom(geom_count,false);
            std::size_t matched=0;
            std::uint64_t matched_indices=0;
            bool ok=true;
            for(std::size_t ci=0;ci<intent_count;++ci){
                const auto wc=window(ci);
                if(base>UINT64_MAX-stride*std::uint64_t(wc.first)){ok=false;break;}
                const std::uint64_t expect=base+stride*std::uint64_t(wc.first);
                unsigned hit=UINT32_MAX;
                for(std::size_t cg=0;cg<geom_count;++cg){
                    const auto vcg=geometry(cg);
                    if(vcg.first==expect&&vcg.second==wc.count){
                        if(hit!=UINT32_MAX){ok=false;break;}
                        hit=unsigned(cg);
                    }
                }
                if(!ok)break;
                if(hit!=UINT32_MAX){
                    if(used_geom[hit]){ok=false;break;}
                    used_geom[hit]=true;
                    cand_map[ci]=hit;
                    ++matched;
                    matched_indices+=wc.count;
                }
            }
            if(!ok)continue;
            if(geom_count<intent_count){
                // Subset BLAS: every BLAS geometry must belong to this intent,
                // and the matched subset must cover a substantial majority of the mesh.
                if(matched!=geom_count)continue;
                if(matched<2&&matched_indices<3000)continue;
                if(matched_indices*2<total_intent_indices)continue;
            }else if(geom_count>intent_count){
                // Superset BLAS: every intent component must be present.
                if(matched!=intent_count)continue;
            }else{
                // Equal count but partial overlap (e.g. swapped out one section):
                // require >= 2 matches and >= 50% index coverage.
                if(matched<2||matched_indices*2<total_intent_indices)continue;
            }
            if(!best_valid||matched>best_matched){
                best_valid=true;ambiguous=false;best_matched=matched;best_base=base;best_map=std::move(cand_map);
            }else if(matched==best_matched&&base!=best_base){
                ambiguous=true;
            }
        }
    }
    if(!best_valid||ambiguous)return false;
    if(mapping)*mapping=std::move(best_map);
    if(ib_base_out)*ib_base_out=best_base;
    if(matched_out)*matched_out=best_matched;
    return true;
}
class NativeDiscovery {
public:
    enum class ReadPoint {upload_copy,rest_consumer,index_consumer};
    enum class Failure:unsigned {none,skin_cb,skin_output,skin_unproven,skin_rest,blas_resource,geometry,window_set,ib_proof,ib_bounds,writer_missing,writer_ambiguous,writer_window,rest_proof,content_hash,component_cover,object_ambiguous,count};
    static constexpr const char *failure_name(Failure f){
        constexpr const char *names[]{"none","skin_cb_unreadable","skin_output_unresolved","skin_receipt_unproven","skin_rest_unresolved","blas_resource_unresolved","geometry_contract","index_window_set","ib_proof_unready","ib_index_bounds","writer_missing","writer_ambiguous","writer_window","rest_proof_unready","content_hash_mismatch","component_cover","object_ambiguous"};return names[unsigned(f)];
    }
    // Observation-only counters. The existing collector mutex owns them;
    // attempts are not unique meshes and never authorize a proof.
    struct Counters {
        std::uint64_t skin_seen=0,skin_facts=0,copy_facts=0,blas_seen=0,builds=0,rest_requests=0,index_requests=0,submitted_builds=0,retired_unproved=0,reset_builds=0,published=0;
        std::array<std::uint64_t,unsigned(Failure::count)> rejected{};
        Failure last_failure=Failure::none,last_resolution_failure=Failure::none;
        unsigned last_wm_guid=0,last_wm_comp=0,last_wm_index=0,last_wm_n=0,last_wm_writers=0,last_wm_res_writers=0;
    };
    struct Bytes {
        std::uint64_t id=0,order=0,scope=0,write_version=0,bindings_revision=UINT64_MAX;NativeRange range;Recording recording;ReadPoint point;
        std::shared_ptr<const std::vector<unsigned char>> data;
        std::uint64_t copy_bytes=0,queue=0,submit_order=0,fence=0,fence_value=0;
        unsigned submissions=0;
        bool gpu=false,issued=false,forwarded=false,valid=true,completed=false,retired=false;
    };
    struct Writer {
        std::uint64_t id=0,order=0;Recording recording;NativeRange output,rest;
        unsigned first=0,count=0;bool extra=false,forwarded=false;
        std::uint64_t queue=0,submit_order=0;unsigned submissions=0;
        std::vector<std::shared_ptr<Bytes>> rest_proofs;
    };
    struct Geometry {
        NativeRange vertices,indices;std::uint64_t vertex_address=0,index_address=0,ib_resource_bytes=0;
        unsigned vertices_count=0,index_count=0,index_stride=4;bool layout_confirmed=false;
    };
    struct Build {
        std::uint64_t id=0,order=0,address=0,queue=0,submit_order=0;Recording recording;
        std::vector<Geometry> geometries;std::vector<Writer> writers;std::vector<NativeMeshIntent> intents;
        std::vector<NativeRange> extra_indices;
        std::vector<std::shared_ptr<Bytes>> index_proofs;
        bool forwarded=false,published=false,discarded=false;unsigned submissions=0;
    };
private:
    std::shared_ptr<NativeProfileRegistry> registry_;
    std::vector<NativeMeshIntent> intents_;
    bool forced_=false;
    Counters counters_;
    bool pending(const NativeMeshIntent &i)const{return forced_||NativeProfileRegistry::resolve(registry_->snapshot(),i).match==NativeProfileRegistry::Match::missing;}
    std::uint64_t serial_=0,order_=0;
    std::vector<Writer> writers_;
    std::vector<std::shared_ptr<Bytes>> bytes_;
    std::vector<std::shared_ptr<Build>> builds_;
    struct RestCandidate {NativeRange range;unsigned vertices=0;};
    std::vector<RestCandidate> rest_candidates_;
    std::shared_ptr<Bytes> request(NativeRange,Recording,ReadPoint,std::uint64_t scope,std::uint64_t order);
    std::shared_ptr<Bytes> proof(NativeRange,const Writer *,const Build &)const;
    struct MissingWriterDiag {unsigned comp=0,index=0,n=0,b_writers=0,res_writers=0;bool set=false;};
    bool resolve(Build &,const NativeMeshIntent &,NativeProfile &,Failure *failure=nullptr,MissingWriterDiag *wm=nullptr)const;
public:
    static constexpr std::uint64_t max_probe_bytes=64ull*1024*1024;
    explicit NativeDiscovery(std::shared_ptr<NativeProfileRegistry> registry):registry_(std::move(registry)){}
    void note_skin(){++counters_.skin_seen;}
    void note_blas(){++counters_.blas_seen;}
    void note_failure(Failure f){if(f!=Failure::none){++counters_.rejected[unsigned(f)];counters_.last_failure=f;}}
    void set_intents(const std::vector<NativeMeshIntent> &values){intents_=values;forced_=false;}
    void force(){forced_=true;}
    bool interested()const{
        if(forced_)return !intents_.empty();const auto state=registry_->snapshot();
        return std::any_of(intents_.begin(),intents_.end(),[&](const auto &i){return NativeProfileRegistry::resolve(state,i).match==NativeProfileRegistry::Match::missing;});
    }
    void written(NativeRange);
    void destroyed(ResourceKey);
    void reset(Recording);
    std::uint64_t skin(Writer);
    void request_known_rest(const Writer &fact_template,std::uint64_t existing_writer_id,std::span<const std::uint64_t> candidate_bytes);
    void skin_forwarded(Recording,bool);
    void copy(NativeRange,Recording,std::span<const unsigned char>);
    bool needs_copy_bytes(NativeRange range)const{
        return std::any_of(rest_candidates_.begin(),rest_candidates_.end(),[&](const auto &r){const auto n=12ull*r.vertices;
            return r.range.resource==range.resource&&r.range.offset==range.offset&&(range.bytes==n||range.bytes==((n+15)&~15ull)||range.bytes==((n+255)&~255ull));});
    }
    void copy_forwarded(Recording,NativeRange);
    std::shared_ptr<Build> build(Recording,std::uint64_t,std::vector<Geometry>);
    void build_forwarded(Recording,std::uint64_t);
    std::vector<std::shared_ptr<Bytes>> requests(Recording,ReadPoint);
    void submission_started(Recording rec){for(auto &p:bytes_)if(p->recording==rec&&p->submissions)p->valid=false;}
    void unknown(){for(auto &p:bytes_)p->valid=false;writers_.clear();rest_candidates_.clear();builds_.clear();}
    ResourceKey resource_of(std::uint64_t id)const{for(const auto &p:bytes_)if(p->id==id&&p->valid)return p->range.resource;return {};}
    bool provide_cpu(std::uint64_t,std::vector<unsigned char>);
    void cancel(std::uint64_t id){for(auto &p:bytes_)if(p->id==id)p->valid=false;}
    void submitted(Recording,std::uint64_t,std::uint64_t);
    void signalled(std::uint64_t,std::uint64_t,std::uint64_t,std::uint64_t);
    bool completed(std::uint64_t,std::uint64_t,std::uint64_t,std::vector<unsigned char>);
    void publish();
    struct Pending {std::size_t builds=0,bytes=0,writers=0,rest_candidates=0;};
    Pending pending_counts()const{return {builds_.size(),bytes_.size(),writers_.size(),rest_candidates_.size()};}
    struct Diagnostics {Counters total;Pending pending;std::size_t missing_intents=0,enabled_missing_intents=0,unsubmitted_builds=0,unforwarded_builds=0;};
    Diagnostics diagnostics()const{
        Diagnostics d{counters_,pending_counts()};const auto state=registry_->snapshot();
        for(const auto &b:builds_){d.unsubmitted_builds+=!b->submissions;d.unforwarded_builds+=!b->forwarded;}
        for(const auto &intent:intents_)if(NativeProfileRegistry::resolve(state,intent).match==NativeProfileRegistry::Match::missing){++d.missing_intents;d.enabled_missing_intents+=intent.enabled;}
        return d;
    }
    std::vector<std::pair<std::shared_ptr<Bytes>,std::shared_ptr<const NativeProfile>>> content_bindings();
};
inline std::shared_ptr<NativeDiscovery::Bytes> NativeDiscovery::request(NativeRange range,Recording rec,ReadPoint point,std::uint64_t scope,std::uint64_t order){
    const unsigned align=point==ReadPoint::index_consumer?2u:4u;
    if(!range.resource||!range.bytes||range.bytes>max_probe_bytes||range.bytes%align||range.offset%align)return {};
    for(auto &p:bytes_)if(p->valid&&!p->retired&&p->point==point&&p->range.resource==range.resource&&p->range.offset==range.offset&&p->range.bytes==range.bytes){
        // Mutable CPU uploads are re-read at this actual consumer, rather than
        // leaving a reusable request that the new recording cannot authorize.
        if(!p->gpu&&p->scope!=scope){p->retired=true;continue;}return p;
    }
    auto p=std::make_shared<Bytes>();p->id=++serial_;p->order=order;p->scope=scope;p->write_version=order_;p->range=range;p->recording=rec;p->point=point;p->gpu=true;bytes_.push_back(p);if(point==ReadPoint::rest_consumer)++counters_.rest_requests;else if(point==ReadPoint::index_consumer)++counters_.index_requests;return p;
}
inline void NativeDiscovery::written(NativeRange range){
    ++order_;
    for(auto &p:bytes_)if(p->valid&&native_overlap(p->range,range))p->valid=false;
    std::erase_if(writers_,[&](const auto &w){return native_overlap(w.output,range);});
}
inline void NativeDiscovery::destroyed(ResourceKey key){
    for(auto &p:bytes_)if(p->range.resource==key)p->valid=false;
    std::erase_if(writers_,[&](const auto &w){return w.output.resource==key||w.rest.resource==key;});
    std::erase_if(rest_candidates_,[&](const auto &r){return r.range.resource==key;});
    std::erase_if(builds_,[&](const auto &b){return std::any_of(b->geometries.begin(),b->geometries.end(),[&](const auto &g){return g.vertices.resource==key||g.indices.resource==key;});});
}
inline void NativeDiscovery::reset(Recording rec){
    std::erase_if(writers_,[&](const auto &w){return w.recording==rec&&w.submissions!=1;});
    if(writers_.size()>2048){
        std::size_t excess=writers_.size()-1024;
        std::erase_if(writers_,[&](const auto &w){if(!excess||w.submissions!=1)return false;--excess;return true;});
    }
    for(auto &p:bytes_)if(p->recording==rec&&!p->submissions)p->valid=false;
    counters_.reset_builds+=std::erase_if(builds_,[&](const auto &b){return b->recording==rec&&!b->submissions;});
}
inline std::uint64_t NativeDiscovery::skin(Writer writer){
    if(!interested())return 0;
    ++counters_.skin_facts;writer.id=++serial_;writer.order=++order_;
    // A BLAS may be recorded on the CPU before its skin list. Only use its
    // full VertexCount as a future rest-range hint, never as writer ordering.
    for(const auto &b:builds_)for(const auto &g:b->geometries){
        const unsigned bn=unsigned(g.vertices.bytes/12);
        if(bn&&native_overlap(writer.output,g.vertices)&&12ull*bn<=writer.rest.bytes){
            const auto found=std::find_if(rest_candidates_.begin(),rest_candidates_.end(),[&](const auto &r){return r.range.resource==writer.rest.resource&&r.range.offset==writer.rest.offset&&r.vertices==bn;});
            if(found==rest_candidates_.end())rest_candidates_.push_back({writer.rest,bn});
        }
    }
    for(const auto &r:rest_candidates_)if(r.range.resource==writer.rest.resource&&r.range.offset==writer.rest.offset&&12ull*r.vertices<=writer.rest.bytes){
        auto range=writer.rest;range.bytes=12ull*r.vertices;request(range,writer.recording,ReadPoint::rest_consumer,writer.id,writer.order);
    }
    for(const auto &p:bytes_)if(p->valid&&p->point!=ReadPoint::index_consumer&&p->range.resource==writer.rest.resource&&p->range.offset==writer.rest.offset&&
        ((p->recording==writer.recording&&p->order<=writer.order)||(p->gpu&&p->completed&&p->submissions==1&&p->forwarded)))writer.rest_proofs.push_back(p);
    const auto id=writer.id;writers_.push_back(std::move(writer));return id;
}
inline void NativeDiscovery::request_known_rest(const Writer &fact_template,std::uint64_t existing_writer_id,std::span<const std::uint64_t> candidate_bytes){
    bool need_request=false;
    for(const auto n:candidate_bytes){
        if(!n||n>fact_template.rest.bytes)continue;
        bool reusable=false;
        for(const auto &p:bytes_)if(p->valid&&!p->retired&&p->point==ReadPoint::rest_consumer&&p->range.resource==fact_template.rest.resource&&p->range.offset==fact_template.rest.offset&&p->range.bytes==n&&p->gpu){reusable=true;break;}
        if(!reusable){need_request=true;break;}
    }
    if(!need_request)return;
    std::uint64_t scope=existing_writer_id;
    std::uint64_t ord=order_;
    if(!scope){
        Writer w=fact_template;w.id=++serial_;w.order=++order_;
        scope=w.id;ord=w.order;
        writers_.push_back(std::move(w));
    }
    for(const auto n:candidate_bytes){
        if(!n||n>fact_template.rest.bytes)continue;
        auto range=fact_template.rest;range.bytes=n;
        request(range,fact_template.recording,ReadPoint::rest_consumer,scope,ord);
    }
}
inline void NativeDiscovery::skin_forwarded(Recording rec,bool forwarded){
    const auto w=std::find_if(writers_.rbegin(),writers_.rend(),[&](const auto &v){return v.recording==rec&&!v.forwarded;});
    if(w==writers_.rend())return;const auto id=w->id;w->forwarded=forwarded;
    for(auto &p:bytes_)if(p->scope==id){p->forwarded=forwarded;if(!forwarded)p->valid=false;}
    if(!forwarded)std::erase_if(writers_,[&](const auto &v){return v.id==id;});
}
inline void NativeDiscovery::copy(NativeRange range,Recording rec,std::span<const unsigned char> data){
    if(!interested()||data.empty()||data.size()!=range.bytes||!needs_copy_bytes(range))return;
    // N is a prior real BLAS candidate. Padding never defines the CRC domain.
    bool wanted=false;
    for(const auto &r:rest_candidates_)if(r.range.resource==range.resource&&r.range.offset==range.offset){
        const auto n=12ull*r.vertices;if(n>data.size()||(range.bytes!=n&&range.bytes!=((n+15)&~15ull)&&range.bytes!=((n+255)&~255ull)))continue;
        const auto hash=native_mesh_content_hash(data.first(std::size_t(n)));
        wanted|=std::any_of(intents_.begin(),intents_.end(),[&](const auto &v){return v.content_hash==hash;});
    }
    if(!wanted){note_failure(Failure::content_hash);return;}
    auto p=std::make_shared<Bytes>();p->id=++serial_;p->order=++order_;p->write_version=order_;p->range=range;p->recording=rec;p->point=ReadPoint::upload_copy;p->copy_bytes=range.bytes;
    p->data=std::make_shared<const std::vector<unsigned char>>(data.begin(),data.end());p->completed=true;bytes_.push_back(std::move(p));++counters_.copy_facts;
}
inline void NativeDiscovery::copy_forwarded(Recording rec,NativeRange range){
    for(auto &p:bytes_)if(p->point==ReadPoint::upload_copy&&p->recording==rec&&p->range.resource==range.resource&&p->range.offset==range.offset&&p->copy_bytes==range.bytes)p->forwarded=true;
}
inline std::vector<std::shared_ptr<NativeDiscovery::Bytes>> NativeDiscovery::requests(Recording rec,ReadPoint point){
    std::vector<std::shared_ptr<Bytes>> out;
    for(auto &p:bytes_)if(p->valid&&!p->retired&&p->gpu&&!p->issued&&p->recording==rec&&p->point==point){p->issued=true;out.push_back(p);}
    return out;
}
inline bool NativeDiscovery::provide_cpu(std::uint64_t id,std::vector<unsigned char> data){
    for(auto &p:bytes_)if(p->id==id&&p->valid&&data.size()==p->range.bytes){p->data=std::make_shared<const std::vector<unsigned char>>(std::move(data));p->gpu=false;p->completed=true;return true;}
    return false;
}
inline std::shared_ptr<NativeDiscovery::Build> NativeDiscovery::build(Recording rec,std::uint64_t address,std::vector<Geometry> geometries){
    if(!interested()||geometries.empty())return {};
    unsigned max_n=0;
    const auto idx_stride=geometries.front().index_stride;
    for(const auto &g:geometries)if(g.vertices_count>max_n)max_n=g.vertices_count;
    if(!max_n||max_n>UINT32_MAX/12||(idx_stride!=2&&idx_stride!=4)){note_failure(Failure::geometry);return {};}
    for(const auto &g:geometries)if(!g.layout_confirmed||!g.vertices_count||g.vertices_count>max_n||g.index_stride!=idx_stride||!g.vertices.resource||!g.indices.resource||
        g.vertices.bytes!=12ull*max_n||g.indices.bytes!=std::uint64_t(idx_stride)*g.index_count||!g.index_count){note_failure(Failure::geometry);return {};}
    auto b=std::make_shared<Build>();b->id=++serial_;b->order=++order_;b->address=address;b->recording=rec;b->geometries=std::move(geometries);
    for(const auto &i:intents_){
        if(!pending(i))continue;
        std::vector<unsigned> mapping;std::uint64_t ib_base=0;
        if(!native_geometry_window_map_subset(i.components.size(),[&](auto c){return i.components[c];},b->geometries.size(),[&](auto c){return std::pair{b->geometries[c].index_address,b->geometries[c].index_count};},&mapping,idx_stride,&ib_base))continue;
        b->intents.push_back(i);
        // For subset BLAS builds (e.g. non-raytraced translucent/eye sections),
        // synthesize and probe the omitted component's index range inside the shared IB resource.
        const Geometry *g0=nullptr;bool ib_shared=true;
        for(std::size_t c=0;c<mapping.size();++c)if(mapping[c]!=UINT32_MAX){
            const auto &gm=b->geometries[mapping[c]];
            if(!g0)g0=&gm;
            else if(gm.indices.resource!=g0->indices.resource||gm.indices.base!=g0->indices.base)ib_shared=false;
        }
        if(g0&&ib_shared&&g0->ib_resource_bytes){
            for(std::size_t c=0;c<mapping.size();++c)if(mapping[c]==UINT32_MAX){
                const auto &w=i.components[c];
                const std::uint64_t off=std::uint64_t(idx_stride)*w.first;
                const std::uint64_t len=std::uint64_t(idx_stride)*w.count;
                if(!len||ib_base>UINT64_MAX-off)continue;
                const std::uint64_t va=ib_base+off;
                if(va<g0->indices.base)continue;
                const std::uint64_t rel=va-g0->indices.base;
                if(rel>g0->ib_resource_bytes||len>g0->ib_resource_bytes-rel)continue;
                const NativeRange ex{g0->indices.resource,rel,len,g0->indices.base};
                const auto same_range=[&](const NativeRange &r){return r.resource==ex.resource&&r.offset==ex.offset&&r.bytes==ex.bytes;};
                if(std::any_of(b->geometries.begin(),b->geometries.end(),[&](const auto &gm){return same_range(gm.indices);}))continue;
                if(std::any_of(b->extra_indices.begin(),b->extra_indices.end(),same_range))continue;
                b->extra_indices.push_back(ex);
            }
        }
    }
    if(b->intents.empty()){note_failure(Failure::window_set);return {};}
    const auto overlaps_build=[&](const Writer &w){
        return std::any_of(b->geometries.begin(),b->geometries.end(),[&](const auto &g){return native_overlap(w.output,g.vertices);});
    };
    for(const auto &w:writers_)if(w.forwarded&&overlaps_build(w)&&((w.recording==rec&&w.order<b->order)||(w.recording!=rec&&w.submissions<=1)))b->writers.push_back(w);
    for(const auto &g:b->geometries){
        request(g.indices,rec,ReadPoint::index_consumer,b->id,b->order);
        for(const auto &w:b->writers)if(native_overlap(w.output,g.vertices)&&12ull*max_n<=w.rest.bytes){
            const auto found=std::find_if(rest_candidates_.begin(),rest_candidates_.end(),[&](const auto &v){return v.range.resource==w.rest.resource&&v.range.offset==w.rest.offset&&v.vertices==max_n;});
            if(found==rest_candidates_.end())rest_candidates_.push_back({w.rest,max_n});
        }
    }
    for(const auto &ex:b->extra_indices)request(ex,rec,ReadPoint::index_consumer,b->id,b->order);
    for(const auto &p:bytes_)if(p->valid&&p->point==ReadPoint::index_consumer&&
        ((p->recording==rec&&p->order<=b->order)||(p->gpu&&p->completed&&p->submissions==1&&p->forwarded)))b->index_proofs.push_back(p);
    builds_.push_back(b);++counters_.builds;return b;
}
inline void NativeDiscovery::build_forwarded(Recording rec,std::uint64_t address){
    for(auto b=builds_.rbegin();b!=builds_.rend();++b)if((*b)->recording==rec&&(*b)->address==address&&!(*b)->forwarded){
        (*b)->forwarded=true;for(auto &p:bytes_)if(p->scope==(*b)->id)p->forwarded=true;return;
    }
}
inline void NativeDiscovery::submitted(Recording rec,std::uint64_t queue,std::uint64_t order){
    for(auto &p:bytes_)if(p->recording==rec){++p->submissions;p->queue=queue;p->submit_order=order;if(p->submissions!=1)p->valid=false;}
    for(auto &w:writers_)if(w.recording==rec){++w.submissions;w.queue=queue;w.submit_order=order;}
    std::erase_if(writers_,[](const auto &w){return w.submissions>1;});
    for(auto &b:builds_){
        if(b->recording==rec){++b->submissions;b->queue=queue;b->submit_order=order;if(b->submissions==1)++counters_.submitted_builds;}
        for(auto &w:b->writers)if(w.recording==rec){++w.submissions;w.queue=queue;w.submit_order=order;}
    }
}
inline void NativeDiscovery::signalled(std::uint64_t id,std::uint64_t queue,std::uint64_t fence,std::uint64_t value){
    for(auto &p:bytes_)if(p->id==id&&p->valid&&p->submissions==1&&p->queue==queue&&fence&&value){p->fence=fence;p->fence_value=value;}
}
inline bool NativeDiscovery::completed(std::uint64_t id,std::uint64_t fence,std::uint64_t value,std::vector<unsigned char> data){
    for(auto &p:bytes_)if(p->id==id&&p->valid&&p->gpu&&p->forwarded&&p->submissions==1&&p->fence==fence&&p->fence_value&&value!=UINT64_MAX&&value>=p->fence_value&&data.size()==p->range.bytes){
        p->data=std::make_shared<const std::vector<unsigned char>>(std::move(data));p->completed=true;return true;
    }
    return false;
}
inline std::shared_ptr<NativeDiscovery::Bytes> NativeDiscovery::proof(NativeRange range,const Writer *writer,const Build &build)const{
    std::shared_ptr<Bytes> result;
    const auto &receipts=writer?writer->rest_proofs:build.index_proofs;
    for(const auto &p:receipts){
        if(!p->valid||!p->forwarded||p->submissions!=1||!p->completed||!p->data||p->range.resource!=range.resource||p->range.offset!=range.offset||p->data->size()<range.bytes)continue;
        if(writer){
            if(p->point==ReadPoint::index_consumer)continue;
            if(p->point==ReadPoint::upload_copy){
                const auto n=range.bytes;
                if(p->copy_bytes!=n&&p->copy_bytes!=((n+15)&~15ull)&&p->copy_bytes!=((n+255)&~255ull))continue;
                // A CPU callback order on two lists/queues is not a GPU edge.
                if(p->recording!=writer->recording||p->order>=writer->order)continue;
            }else if(p->range.bytes!=range.bytes)continue;
        }else if(p->point!=ReadPoint::index_consumer||p->range.bytes!=range.bytes)continue;
        if(result&&std::memcmp(result->data->data(),p->data->data(),std::size_t(range.bytes)))return {};
        result=p;
    }
    return result;
}
inline bool NativeDiscovery::resolve(Build &b,const NativeMeshIntent &intent,NativeProfile &out,Failure *failure,MissingWriterDiag *wm)const{
    const auto reject=[&](Failure f){if(failure)*failure=f;return false;};
    const auto idx_stride=b.geometries.front().index_stride;
    std::vector<unsigned> mapping;std::uint64_t ib_base=0;
    if(!native_geometry_window_map_subset(intent.components.size(),[&](auto c){return intent.components[c];},b.geometries.size(),[&](auto c){return std::pair{b.geometries[c].index_address,b.geometries[c].index_count};},&mapping,idx_stride,&ib_base))return reject(Failure::window_set);
    const unsigned n=unsigned(b.geometries.front().vertices.bytes/12);
    // If any intent component was omitted from the RT BLAS, require all present
    // geometries to share a single VB and IB allocation.
    const Geometry *shared_g=nullptr;bool shared_ok=true;bool has_omitted=false;
    for(std::size_t c=0;c<mapping.size();++c){
        if(mapping[c]==UINT32_MAX){has_omitted=true;continue;}
        const auto &gm=b.geometries[mapping[c]];
        if(!shared_g)shared_g=&gm;
        else if(gm.vertices.resource!=shared_g->vertices.resource||gm.vertex_address!=shared_g->vertex_address||
                gm.indices.resource!=shared_g->indices.resource||gm.indices.base!=shared_g->indices.base)shared_ok=false;
    }
    if(has_omitted&&(!shared_g||!shared_ok||!shared_g->ib_resource_bytes))return reject(Failure::geometry);
    out={};out.object_guid=intent.object_guid;out.content_hash=intent.content_hash;out.vertices=n;
    for(std::size_t c=0;c<mapping.size();++c){
        NativeRange c_ib_range{};unsigned c_index_count=0;ResourceKey c_vb_res{};std::uint64_t c_vertex_address=0;
        if(mapping[c]!=UINT32_MAX){
            const auto &g=b.geometries[mapping[c]];
            c_ib_range=g.indices;c_index_count=g.index_count;c_vb_res=g.vertices.resource;c_vertex_address=g.vertex_address;
        }else{
            const auto &w=intent.components[c];
            const std::uint64_t off=std::uint64_t(idx_stride)*w.first;
            const std::uint64_t len=std::uint64_t(idx_stride)*w.count;
            if(!len||ib_base>UINT64_MAX-off)return reject(Failure::ib_proof);
            const std::uint64_t va=ib_base+off;
            if(va<shared_g->indices.base)return reject(Failure::ib_proof);
            const std::uint64_t rel=va-shared_g->indices.base;
            if(rel>shared_g->ib_resource_bytes||len>shared_g->ib_resource_bytes-rel)return reject(Failure::ib_proof);
            c_ib_range={shared_g->indices.resource,rel,len,shared_g->indices.base};
            c_index_count=w.count;c_vb_res=shared_g->vertices.resource;c_vertex_address=shared_g->vertex_address;
        }
        const auto ib=proof(c_ib_range,nullptr,b);
        if(!ib)return reject(Failure::ib_proof);
        const Writer *owner=nullptr;
        for(unsigned i=0;i<c_index_count;++i){
            std::uint32_t index=0;
            if(idx_stride==2){std::uint16_t idx16=0;std::memcpy(&idx16,ib->data->data()+2ull*i,2);index=idx16;}
            else std::memcpy(&index,ib->data->data()+4ull*i,4);
            if(index>=n||c_vertex_address>UINT64_MAX-12ull*index)return reject(Failure::ib_bounds);
            const auto address=c_vertex_address+12ull*index;
            const Writer *at=nullptr;bool at_same_rec=false;std::uint64_t at_major=0,at_minor=0;
            for(const auto &w:b.writers){
                if(!w.forwarded)continue;
                const bool same_rec=(w.recording==b.recording&&w.order<b.order);
                const bool cross_rec=(w.recording!=b.recording&&w.submissions==1&&b.submissions==1&&w.queue&&w.queue==b.queue&&w.submit_order<b.submit_order);
                if(!same_rec&&!cross_rec)continue;
                if(w.output.resource!=c_vb_res||w.output.base>UINT64_MAX-w.output.offset||
                   address<w.output.base+w.output.offset||w.output.bytes<12||
                   address-w.output.base-w.output.offset>w.output.bytes-12)continue;
                const std::uint64_t major=same_rec?w.order:w.submit_order;
                const std::uint64_t minor=w.order;
                if(!at){at=&w;at_same_rec=same_rec;at_major=major;at_minor=minor;}
                else if(at->id!=w.id){
                    const bool newer=(same_rec!=at_same_rec)?same_rec:(major!=at_major?major>at_major:minor>at_minor);
                    const bool older=(same_rec!=at_same_rec)?at_same_rec:(major!=at_major?major<at_major:minor<at_minor);
                    if(newer){at=&w;at_same_rec=same_rec;at_major=major;at_minor=minor;}
                    else if(!older)return reject(Failure::writer_ambiguous);
                }
            }
            if(!at){
                if(wm){
                    unsigned res_w=0;
                    for(const auto &w:b.writers)if(w.output.resource==c_vb_res)++res_w;
                    *wm={intent.components[c].component,index,n,unsigned(b.writers.size()),res_w,true};
                }
                return reject(Failure::writer_missing);
            }
            if(owner&&owner->id!=at->id)return reject(Failure::writer_ambiguous);
            owner=at;
        }
        if(!owner||!owner->count||owner->first>n||owner->count>n-owner->first||owner->output.bytes!=12ull*owner->count||
            c_vertex_address>UINT64_MAX-12ull*owner->first||owner->output.base+owner->output.offset!=c_vertex_address+12ull*owner->first)return reject(Failure::writer_window);
        auto rest=owner->rest;rest.bytes=12ull*n;if(owner->rest.bytes<rest.bytes)return reject(Failure::rest_proof);
        const auto values=proof(rest,owner,b);
        if(!values)return reject(Failure::rest_proof);
        if(native_mesh_content_hash(std::span<const unsigned char>(values->data->data(),std::size_t(rest.bytes)))!=intent.content_hash)return reject(Failure::content_hash);
        const auto &w=intent.components[c];out.components.push_back({w.component,owner->first,owner->count,w.first,w.count});
    }
    std::sort(out.components.begin(),out.components.end(),[](const auto &a,const auto &v){return a.first<v.first;});
    unsigned next=0;
    for(std::size_t i=0;i<out.components.size();++i){const auto &c=out.components[i];if(c.first!=next)return reject(Failure::component_cover);next+=c.count;
        for(std::size_t j=0;j<i;++j)if(out.components[j].component==c.component)return reject(Failure::component_cover);
    }
    if(next!=n)return reject(Failure::component_cover);return true;
}
inline void NativeDiscovery::publish(){
    for(auto &b:builds_)if(!b->published&&!b->discarded&&b->forwarded&&b->submissions==1){
        std::erase_if(b->intents,[&](const auto &i){return !pending(i);});
        if(b->intents.empty()){b->discarded=true;continue;}
        // Merge any same-queue earlier-submitted overlapping writers recorded on other command lists.
        for(const auto &w:writers_){
            if(!w.forwarded||w.submissions!=1||!w.queue||w.queue!=b->queue||w.submit_order>=b->submit_order)continue;
            const bool overlaps=std::any_of(b->geometries.begin(),b->geometries.end(),[&](const auto &g){return native_overlap(w.output,g.vertices);});
            if(!overlaps)continue;
            if(std::none_of(b->writers.begin(),b->writers.end(),[&](const auto &e){return e.id==w.id;}))b->writers.push_back(w);
        }
        std::vector<NativeProfile> candidates;
        bool any_pending_failure=false;
        for(const auto &intent:b->intents){
            NativeProfile p;Failure failure=Failure::none;MissingWriterDiag wm{};
            if(resolve(*b,intent,p,&failure,&wm))candidates.push_back(std::move(p));
            else{
                note_failure(failure);counters_.last_resolution_failure=failure;
                if(failure==Failure::ib_proof||failure==Failure::rest_proof)any_pending_failure=true;
                if(failure==Failure::writer_missing&&wm.set){
                    counters_.last_wm_guid=intent.object_guid;counters_.last_wm_comp=wm.comp;
                    counters_.last_wm_index=wm.index;counters_.last_wm_n=wm.n;
                    counters_.last_wm_writers=wm.b_writers;counters_.last_wm_res_writers=wm.res_writers;
                }
            }
        }
        if(candidates.empty()){
            bool waiting=false,impossible=!any_pending_failure;
            if(!impossible){
                for(const auto &g:b->geometries){
                    bool available=false;for(const auto &p:b->index_proofs)if(p->valid&&p->range.resource==g.indices.resource&&p->range.offset==g.indices.offset&&p->range.bytes==g.indices.bytes){available=true;waiting|=p->gpu&&!p->completed;}
                    impossible|=!available;
                }
                for(const auto &ex:b->extra_indices){
                    bool available=false;for(const auto &p:b->index_proofs)if(p->valid&&p->range.resource==ex.resource&&p->range.offset==ex.offset&&p->range.bytes==ex.bytes){available=true;waiting|=p->gpu&&!p->completed;}
                    impossible|=!available;
                }
                bool has_preceding_writer=false,has_writer_rest=false;
                for(const auto &w:b->writers){
                    const bool same_rec=(w.recording==b->recording&&w.order<b->order);
                    const bool cross_rec=(w.recording!=b->recording&&w.submissions==1&&b->submissions==1&&w.queue&&w.queue==b->queue&&w.submit_order<b->submit_order);
                    if(!same_rec&&!cross_rec)continue;
                    has_preceding_writer=true;
                    for(const auto &p:w.rest_proofs)if(p->valid){has_writer_rest=true;waiting|=p->gpu&&!p->completed;}
                }
                if(!has_preceding_writer||!has_writer_rest)impossible=true;
            }
            if(impossible)waiting=false;
            if(!waiting){b->discarded=true;++counters_.retired_unproved;}
            continue;
        }
        if(std::any_of(candidates.begin(),candidates.end(),[&](const auto &p){return p.object_guid!=candidates.front().object_guid;})){b->discarded=true;note_failure(Failure::object_ambiguous);continue;}
        for(const auto &p:candidates)if(registry_->publish(p).inserted)++counters_.published;
        b->published=true;
    }
    std::erase_if(builds_,[](const auto &b){return b->published||b->discarded||b->submissions>1;});
    if(!interested()){
        rest_candidates_.clear();
        std::erase_if(writers_,[](const auto &w){return w.forwarded;});
        for(auto &b:builds_)b->discarded=true;
        std::erase_if(builds_,[](const auto &b){return b->discarded;});
        for(auto &p:bytes_)if(p->point==ReadPoint::index_consumer)p->retired=true;
    }
    std::erase_if(bytes_,[](const auto &p){return !p->valid||(p->retired&&p.use_count()==1);});
}
inline std::vector<std::pair<std::shared_ptr<NativeDiscovery::Bytes>,std::shared_ptr<const NativeProfile>>> NativeDiscovery::content_bindings(){
    std::vector<std::pair<std::shared_ptr<Bytes>,std::shared_ptr<const NativeProfile>>> out;const auto state=registry_->snapshot();
    for(const auto &p:bytes_)if(p->bindings_revision!=state->revision&&p->valid&&p->forwarded&&p->submissions==1&&p->completed&&p->data&&p->point!=ReadPoint::index_consumer){
        p->bindings_revision=state->revision;
        for(const auto &native:state->profiles){const auto n=native->bytes();if(p->data->size()<n)continue;
            if(p->point==ReadPoint::upload_copy&&p->copy_bytes!=n&&p->copy_bytes!=((n+15)&~15ull)&&p->copy_bytes!=((n+255)&~255ull))continue;
            if(p->point!=ReadPoint::upload_copy&&p->range.bytes!=n)continue;
            if(native_mesh_content_hash(std::span<const unsigned char>(p->data->data(),std::size_t(n)))==native->content_hash)out.emplace_back(p,native);
        }
    }
    if(!interested())for(auto &p:bytes_)if(p->point==ReadPoint::index_consumer||(p->point==ReadPoint::upload_copy&&p->bindings_revision==state->revision))p->retired=true;
    return out;
}
}
