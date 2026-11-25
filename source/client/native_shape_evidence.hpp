#pragma once
#include "hsin_pose.hpp"
#include <deque>
#include <atomic>
#include <cstring>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <tuple>
#include <set>
#include <sstream>
namespace hsin_rt {
struct ShapeEvidenceRange {
    unsigned parameter=0,type=0,binding=0,reg=0,space=0,count=0,visibility=0,param_type=0;
    bool root=false;unsigned array_size=1,flags=0;bool flags_known=false;
};
struct ShapeEvidenceLayout {
    std::uint64_t handle=0,creation=0;unsigned parameters=0;
    std::vector<ShapeEvidenceRange> ranges;std::string file,json;
};
struct ShapeEvidenceCode {std::uint64_t hash=0;std::string file;std::vector<unsigned char> bytes;
    std::shared_ptr<std::atomic<std::size_t>> retained;
    ~ShapeEvidenceCode(){if(retained)retained->fetch_sub(bytes.size(),std::memory_order_relaxed);}
};
struct ShapeEvidenceShader {
    std::uint64_t handle=0,creation=0;
    std::shared_ptr<ShapeEvidenceLayout> layout;std::shared_ptr<ShapeEvidenceCode> code;
};
struct ShapeEvidenceBinding {
    ShapeEvidenceRange declaration;unsigned slot_in_range=0,status=2,root_kind=0,format=0,stride=0,descriptor_version=0;
    std::uint64_t argument=0,query_epoch=0,heap=0,slot=0,resource=0,creation=0,offset=0,bytes=0;
    unsigned wrote_frame=0,wrote_source=0;bool extent_unknown=false,uav=false;
};
// Passive callback data, not a writer/queue scheduler. No CB/resource reads,
// GPU commands, FNV role selection, or changes to rendering admission.
class NativeShapeEvidence {
public:
    struct File {std::string name;std::shared_ptr<ShapeEvidenceCode> code;std::string text;};
    struct Batch {std::filesystem::path directory;std::vector<File> files;std::vector<std::string> lines;};
    struct Fact {
        Recording recording;std::uint64_t call=0,order=0,tick=0,recording_ring_evictions=0;
        unsigned flags=0,x=0,y=0,z=0,phase=1;unsigned non_buffer_ranges_omitted=0;bool retained_view_snapshot=false,bindings_truncated=false,pso_from_last_bind=false,layout_from_pipeline_creation=false;
        std::shared_ptr<ShapeEvidenceShader> shader;std::shared_ptr<ShapeEvidenceLayout> layout;
        std::vector<ShapeEvidenceBinding> bindings;
        unsigned object=0,content_hash=0,component=0;NativeShapeRead read;
    };
private:
    struct Ring {Recording recording;std::deque<Fact> facts;std::uint64_t dropped=0;};
    std::filesystem::path directory_;std::map<std::uint64_t,Ring> rings_;
    using ResourceIdentity=std::pair<std::uint64_t,std::uint64_t>;
    using ViewIdentity=std::tuple<std::uint64_t,std::uint64_t,std::uint64_t,std::uint64_t>;
    std::map<ResourceIdentity,std::map<ViewIdentity,std::shared_ptr<Fact>>> view_snapshots_;
    std::uint64_t view_snapshot_count_=0,view_snapshot_overflow_=0;
    std::multimap<std::uint64_t,std::weak_ptr<ShapeEvidenceCode>> codes_;
    std::set<std::string> layouts_saved_,codes_saved_;
    std::map<std::string,unsigned> targets_quality_;
    std::map<std::string,unsigned> candidates_quality_;
    std::vector<File> files_;std::vector<std::string> lines_;
    std::uint64_t serial_=0,creation_start_=0,start_=0,end_=0,order_=0,summary_due_=0;
    std::uint64_t cs_created_=0,container_bytes_=0,missing_code_=0,cache_overflow_=0,ring_drops_=0,reset_retired_facts_=0,replaced_recording_retired_facts_=0,targets_=0,candidates_=0,unmatched_=0,output_drops_=0;
    std::shared_ptr<std::atomic<std::size_t>> retained_bytes_=std::make_shared<std::atomic<std::size_t>>(0);bool enabled_=false,ended_=false,dirty_=false;
    static constexpr std::size_t cache_pending_limit=64*1024*1024,ring_limit=128,candidate_limit=128;
    static std::string hx(std::uint64_t n){std::ostringstream s;s<<std::hex<<n;return s.str();}
    static std::string q(const std::string &v){std::string s="\"";for(char c:v){if(c=='\\'||c=='\"')s+='\\';if(c=='\n')s+="\\n";else if(c=='\r')s+="\\r";else s+=c;}return s+'\"';}
    static std::string rec(Recording r){return "[\""+hx(r.list)+"\","+std::to_string(r.creation)+","+std::to_string(r.generation)+"]";}
    static bool overlap(std::uint64_t a,std::uint64_t n,std::uint64_t b,std::uint64_t m){return n&&m&&(a>=b?a-b<m:b-a<n);}
    std::string serialize(const Fact &,const Fact &,bool)const;
    void target(Fact &,std::uint64_t);
    void remember_views(const Fact &);
    void export_files(const Fact &);
    std::string serialize_target(const Fact &)const;
public:
    bool enabled()const noexcept{return enabled_;}
    bool wants_commands()const noexcept{return enabled_&&!ended_;}
    std::uint64_t next_creation()noexcept{return ++serial_;}
    void configure(const std::filesystem::path &directory){directory_=directory;enabled_=!directory.empty();if(enabled_){creation_start_=GetTickCount64();dirty_=true;}}
    std::shared_ptr<ShapeEvidenceLayout> layout(std::uint64_t handle,unsigned parameters,std::vector<ShapeEvidenceRange>);
    std::shared_ptr<ShapeEvidenceShader> shader(std::uint64_t handle,std::uint64_t creation,std::shared_ptr<ShapeEvidenceLayout>,const void *,std::size_t);
    void before(Fact);
    void mark_target(Recording,unsigned object,unsigned hash,unsigned component,NativeShapeRead);
    void after(Recording,std::uint64_t call,unsigned phase);
    void reset(Recording r){if(auto p=rings_.find(r.list);p!=rings_.end()&&p->second.recording==r){reset_retired_facts_+=p->second.facts.size();dirty_=true;rings_.erase(p);}}
    void destroyed(ResourceKey r){if(!enabled_)return;const auto p=view_snapshots_.find({r.object,r.creation});if(p!=view_snapshots_.end()){view_snapshot_count_-=p->second.size();view_snapshots_.erase(p);dirty_=true;}}
    void submitted(Recording,std::uint64_t queue);
    Batch take(std::uint64_t now);
    static std::string write(Batch);
};
inline std::shared_ptr<ShapeEvidenceLayout> NativeShapeEvidence::layout(std::uint64_t handle,unsigned count,std::vector<ShapeEvidenceRange> ranges){
    if(!enabled_)return {};auto p=std::make_shared<ShapeEvidenceLayout>();p->handle=handle;p->creation=next_creation();p->parameters=count;p->ranges=std::move(ranges);
    p->file="layout-"+std::to_string(p->creation)+".json";
    std::ostringstream s;s<<"{\"handle\":"<<q(hx(handle))<<",\"creation\":"<<p->creation<<",\"raw_root_flags\":null,\"raw_root_flags_state\":\"unobserved\",\"parameters\":"<<count<<",\"ranges\":[";bool comma=false;
    for(const auto &d:p->ranges){if(comma)s<<',';comma=true;s<<"{\"parameter\":"<<d.parameter<<",\"param_type\":"<<d.param_type<<",\"type\":"<<d.type<<",\"binding\":"<<d.binding<<",\"register\":"<<d.reg<<",\"space\":"<<d.space<<",\"count\":"<<d.count<<",\"visibility\":"<<d.visibility<<",\"root\":"<<d.root<<",\"array_size\":"<<d.array_size<<",\"range_flags\":"<<d.flags<<",\"range_flags_known\":"<<d.flags_known<<'}';}
    s<<"]}";p->json=s.str();return p;
}
inline std::shared_ptr<ShapeEvidenceShader> NativeShapeEvidence::shader(std::uint64_t handle,std::uint64_t creation,std::shared_ptr<ShapeEvidenceLayout> layout,const void *code,std::size_t size){
    if(!enabled_)return {};std::erase_if(codes_,[](const auto &entry){return entry.second.expired();});auto p=std::make_shared<ShapeEvidenceShader>();p->handle=handle;p->creation=creation;p->layout=std::move(layout);++cs_created_;dirty_=true;
    if(!code||!size){++missing_code_;return p;}
    auto h=0xcbf29ce484222325ull;const auto *b=static_cast<const unsigned char *>(code);for(std::size_t i=0;i<size;++i)h=(h^b[i])*0x100000001b3ull;
    const auto [a,z]=codes_.equal_range(h);for(auto i=a;i!=z;++i)if(auto old=i->second.lock();old&&old->bytes.size()==size&&!std::memcmp(old->bytes.data(),b,size)){p->code=std::move(old);break;}
    if(!p->code){
        if(size>cache_pending_limit-retained_bytes_->load(std::memory_order_relaxed)){++cache_overflow_;return p;}
        auto blob=std::make_shared<ShapeEvidenceCode>();blob->hash=h;blob->file="cs-"+std::to_string(next_creation())+".cso";blob->bytes.assign(b,b+size);
        blob->retained=retained_bytes_;retained_bytes_->fetch_add(size,std::memory_order_relaxed);p->code=blob;codes_.emplace(h,blob);container_bytes_+=size;
    }
    return p;
}
inline void NativeShapeEvidence::export_files(const Fact &f){
    if(f.layout&&layouts_saved_.insert(f.layout->file).second)files_.push_back({f.layout->file,{},f.layout->json});
    if(f.shader&&f.shader->code&&codes_saved_.insert(f.shader->code->file).second)files_.push_back({f.shader->code->file,f.shader->code,{}});
}
inline std::string NativeShapeEvidence::serialize(const Fact &f,const Fact &t,bool local)const{
    std::ostringstream s;s<<"{\"type\":\"candidate\",\"proof\":\"UAV overlap only; role/write/GPU completion unproved\",\"local_recording\":"<<local<<",\"predecessors_lost\":"<<t.recording_ring_evictions<<",\"producer_recording_ring_evictions_at_observation\":"<<f.recording_ring_evictions
        <<",\"target_recording_ring_evictions\":"<<t.recording_ring_evictions<<",\"object\":"<<t.object<<",\"hash\":"<<q(hx(t.content_hash))<<",\"component\":"<<t.component<<",\"target_recording\":"<<rec(t.recording)<<",\"target_call\":"<<t.call<<",\"target_phase\":"<<t.phase
        <<",\"t3\":[\""<<hx(t.read.resource.object)<<"\","<<t.read.resource.creation<<","<<t.read.offset<<","<<t.read.bytes<<"]"
        <<",\"producer_recording\":"<<rec(f.recording)<<",\"call\":"<<f.call<<",\"order\":"<<f.order<<",\"phase\":"<<f.phase<<",\"flags\":"<<f.flags<<",\"groups\":["<<f.x<<","<<f.y<<","<<f.z<<"]"
        <<",\"pso\":"<<q(hx(f.shader?f.shader->handle:0))<<",\"pso_creation\":"<<(f.shader?f.shader->creation:0)<<",\"layout_creation\":"<<(f.layout?f.layout->creation:0)<<",\"retained_view_snapshot\":"<<f.retained_view_snapshot<<",\"pso_from_last_bind\":"<<f.pso_from_last_bind
        <<",\"layout_from_pipeline_creation\":"<<f.layout_from_pipeline_creation<<",\"container\":"<<q(f.shader&&f.shader->code?f.shader->code->file:"")<<",\"layout\":"<<q(f.layout?f.layout->file:"")<<",\"non_buffer_ranges_omitted\":"<<f.non_buffer_ranges_omitted<<",\"bindings_truncated\":"<<f.bindings_truncated<<",\"bindings\":[";
    bool comma=false;for(const auto &b:f.bindings){if(comma)s<<',';comma=true;const auto &d=b.declaration;
        s<<"{\"parameter\":"<<d.parameter<<",\"type\":"<<d.type<<",\"register\":"<<d.reg+b.slot_in_range<<",\"space\":"<<d.space<<",\"declared_count\":"<<d.count<<",\"root\":"<<d.root
         <<",\"status\":"<<b.status<<",\"root_kind\":"<<b.root_kind<<",\"argument\":"<<q(hx(b.argument))<<",\"query_epoch\":"<<b.query_epoch<<",\"heap\":"<<q(hx(b.heap))<<",\"slot\":"<<b.slot
         <<",\"heap_creation\":null,\"descriptor_write_generation\":null,\"descriptor_version\":"<<b.descriptor_version<<",\"wrote_frame\":"<<b.wrote_frame<<",\"wrote_source\":"<<b.wrote_source
         <<",\"resource\":"<<q(hx(b.resource))<<",\"resource_creation\":"<<b.creation<<",\"offset\":"<<b.offset<<",\"bytes\":"<<b.bytes<<",\"extent_unknown\":"<<b.extent_unknown<<",\"format\":"<<b.format<<",\"stride\":"<<b.stride<<'}';
    }s<<"]}";return s.str();
}
inline std::string NativeShapeEvidence::serialize_target(const Fact &t)const{
    auto row=serialize(t,t,true);row.replace(row.find("candidate"),9,"target_read");
    const auto proof=row.find("UAV overlap only; role/write/GPU completion unproved");
    row.replace(proof,std::strlen("UAV overlap only; role/write/GPU completion unproved"),"recognized SkinCache read; CPU bindings observed; GPU completion unproved");return row;
}
inline void NativeShapeEvidence::remember_views(const Fact &f){
    if(!f.shader)return;std::shared_ptr<Fact> copy;
    for(const auto &b:f.bindings)if(b.uav&&!b.status&&b.resource&&b.creation&&b.bytes){
        const ResourceIdentity resource{b.resource,b.creation};auto ri=view_snapshots_.find(resource);
        const ViewIdentity key{b.offset,b.bytes,f.shader->creation,f.layout?f.layout->creation:0};
        if(ri!=view_snapshots_.end())if(const auto old=ri->second.find(key);old!=ri->second.end()){
            auto &prior=*old->second;if(prior.order>f.order)continue;
            if(prior.order==f.order&&prior.recording==f.recording&&prior.call==f.call){
                prior.phase=f.phase;prior.flags=f.flags;prior.object=f.object;prior.content_hash=f.content_hash;prior.component=f.component;prior.read=f.read;continue;
            }
        }
        const bool fresh=ri==view_snapshots_.end()||!ri->second.contains(key);
        if(fresh&&ri!=view_snapshots_.end()&&ri->second.size()>=128){++view_snapshot_overflow_;dirty_=true;continue;}
        if(!copy){copy=std::make_shared<Fact>(f);copy->retained_view_snapshot=true;}
        if(ri==view_snapshots_.end())ri=view_snapshots_.try_emplace(resource).first;
        if(fresh)++view_snapshot_count_;ri->second[key]=copy;dirty_=true;
    }
}
inline void NativeShapeEvidence::before(Fact f){
    if(!wants_commands())return;auto &r=rings_[f.recording.list];if(r.recording!=f.recording){replaced_recording_retired_facts_+=r.facts.size();r={};r.recording=f.recording;}
    f.order=++order_;f.tick=GetTickCount64();if(r.facts.size()>=ring_limit){r.facts.pop_front();++r.dropped;++ring_drops_;}f.recording_ring_evictions=r.dropped;remember_views(f);r.facts.push_back(std::move(f));
}
inline void NativeShapeEvidence::mark_target(Recording r,unsigned object,unsigned hash,unsigned component,NativeShapeRead read){
    if(!wants_commands())return;const auto i=rings_.find(r.list);if(i==rings_.end()||i->second.recording!=r||i->second.facts.empty())return;
    auto &f=i->second.facts.back();f.object=object;f.content_hash=hash;f.component=component;f.read=read;
}
inline void NativeShapeEvidence::after(Recording r,std::uint64_t call,unsigned phase){
    if(!wants_commands())return;const auto i=rings_.find(r.list);if(i==rings_.end()||i->second.recording!=r)return;
    for(auto f=i->second.facts.rbegin();f!=i->second.facts.rend();++f)if(f->call==call){f->phase=phase;remember_views(*f);if(f->object&&phase==2)target(*f,GetTickCount64());return;}
}
inline void NativeShapeEvidence::target(Fact &t,std::uint64_t now){
    if(ended_)return;if(!start_){start_=now;end_=now+60000;summary_due_=0;dirty_=true;}
    if(now>=end_){ended_=true;dirty_=true;return;}++targets_;bool found=false;
    const auto quality_of=[](const Fact &f){unsigned value=(f.phase==2?0x10000u:0)+(f.flags&1?0x1000u:0)+(f.flags&2?0x800u:0)+(!f.bindings_truncated?0x400u:0);
        for(const auto &b:f.bindings)if(!b.status)value+=1+(b.resource&&b.creation?4:0)+(b.bytes&&!b.extent_unknown?2:0);return value;};
    const auto target_key=std::to_string(t.object)+":"+std::to_string(t.component)+":"+std::to_string(t.shader?t.shader->creation:0)+":"+std::to_string(t.layout?t.layout->creation:0)+":"+std::to_string(t.read.resource.object)+":"+std::to_string(t.read.resource.creation)+":"+std::to_string(t.read.offset)+":"+std::to_string(t.read.bytes);
    const auto tq=quality_of(t)+(t.read.resource&&t.read.bytes?0x20000u:0);const auto old_target=targets_quality_.find(target_key);
    if(old_target==targets_quality_.end()||old_target->second<tq){
        if(old_target==targets_quality_.end()&&targets_quality_.size()>=candidate_limit)++output_drops_;
        else {targets_quality_[target_key]=tq;export_files(t);lines_.push_back(serialize_target(t));}
    }

    struct Range {std::uint64_t resource,creation,offset,bytes;};
    std::vector<Range> relevant{{t.read.resource.object,t.read.resource.creation,t.read.offset,t.read.bytes}};
    std::set<std::tuple<std::uint64_t,std::uint64_t,std::uint64_t,std::uint64_t,std::uint64_t>> visited;
    const auto consider=[&](const Fact &f){
        if((f.recording==t.recording&&f.call==t.call)||!f.shader)return;const bool local=f.recording==t.recording;if(local&&f.order>=t.order)return;
        bool match=false;for(const auto &b:f.bindings)if(b.uav&&!b.status)for(const auto &a:relevant)
            if(b.resource==a.resource&&b.creation==a.creation&&overlap(b.offset,b.bytes,a.offset,a.bytes)){match=true;break;}
        if(!match)return;
        const auto identity=std::make_tuple(f.recording.list,f.recording.creation,f.recording.generation,f.call,f.order);
        if(!visited.insert(identity).second)return;found=true;
        for(const auto &b:f.bindings)if(!b.status&&b.resource&&b.creation&&b.bytes&&!b.extent_unknown&&b.root_kind!=2&&relevant.size()<128)
            relevant.push_back({b.resource,b.creation,b.offset,b.bytes});
        const auto key=std::to_string(t.object)+":"+std::to_string(f.shader->creation)+":"+std::to_string(f.layout?f.layout->creation:0)+":"+std::to_string(f.x)+":"+std::to_string(f.y)+":"+std::to_string(f.z)+":"+std::to_string(local);
        unsigned quality=(f.phase==2?0x10000u:0)+(f.flags&1?0x1000u:0)+(f.flags&2?0x800u:0)+(!f.bindings_truncated?0x400u:0);
        for(const auto &b:f.bindings)if(!b.status)quality+=1+(b.resource&&b.creation?4:0)+(b.bytes&&!b.extent_unknown?2:0);
        const auto saved=candidates_quality_.find(key);if(saved!=candidates_quality_.end()&&saved->second>=quality)return;
        if(saved==candidates_quality_.end()&&candidates_quality_.size()>=candidate_limit){++output_drops_;return;}
        candidates_quality_[key]=quality;export_files(f);export_files(t);auto row=serialize(f,t,local);row.pop_back();
        row+=",\"target_fact\":"+serialize_target(t)+"}";lines_.push_back(std::move(row));++candidates_;dirty_=true;
    };
    std::vector<const Ring *> order;const auto own=rings_.find(t.recording.list);if(own!=rings_.end())order.push_back(&own->second);
    for(const auto &[_,r]:rings_)if(&r!=(own==rings_.end()?nullptr:&own->second))order.push_back(&r);
    for(const auto *ring:order)for(auto it=ring->facts.rbegin();it!=ring->facts.rend();++it)consider(*it);
    // A resource's latest observed view facts are independent of unrelated
    // resources' traffic. They remain candidates, never content/queue proof.
    for(std::size_t edge=0;edge<relevant.size()&&edge<128;++edge){const auto a=relevant[edge];
        const auto resource=view_snapshots_.find({a.resource,a.creation});if(resource==view_snapshots_.end())continue;
        for(const auto &[view,f]:resource->second)if(overlap(std::get<0>(view),std::get<1>(view),a.offset,a.bytes))
            consider(*f);
    }
    if(!found){++unmatched_;dirty_=true;}
}
inline void NativeShapeEvidence::submitted(Recording r,std::uint64_t queue){
    if(!enabled_||!start_||ended_)return;const auto it=rings_.find(r.list);if(it==rings_.end()||it->second.recording!=r||
        std::none_of(it->second.facts.begin(),it->second.facts.end(),[](const auto &f){return f.object!=0;}))return;
    std::ostringstream s;s<<"{\"type\":\"native_submit\",\"recording\":"<<rec(r)<<",\"queue\":"<<q(hx(queue))<<",\"gpu_completion\":false}";
    if(lines_.size()<512)lines_.push_back(s.str());else ++output_drops_;
}
inline NativeShapeEvidence::Batch NativeShapeEvidence::take(std::uint64_t now){
    Batch out;if(!enabled_)return out;out.directory=directory_;
    if(start_&&now>=end_&&!ended_){ended_=true;dirty_=true;}
    if(dirty_&&(!summary_due_||now>=summary_due_)){
        std::ostringstream s;s<<"{\"type\":\"coverage\",\"mode\":\"passive_cpu_metadata\",\"creation_start_ms\":"<<creation_start_<<",\"observation_start_ms\":"<<start_<<",\"observation_end_ms\":"<<(ended_?end_:0)
            <<",\"now_ms\":"<<now<<",\"stage\":\""<<(!start_?"waiting_for_shapekey_consumer":ended_?"window_ended":"target_window")<<"\",\"cs_created\":"<<cs_created_<<",\"container_bytes\":"<<container_bytes_
            <<",\"missing_container\":"<<missing_code_<<",\"cache_overflow\":"<<cache_overflow_<<",\"retained_container_bytes\":"<<retained_bytes_->load(std::memory_order_relaxed)<<",\"rolling_predecessors_lost\":"<<ring_drops_<<",\"reset_retired_facts\":"<<reset_retired_facts_<<",\"replaced_recording_retired_facts\":"<<replaced_recording_retired_facts_<<",\"retained_view_snapshots\":"<<view_snapshot_count_<<",\"view_snapshot_overflow\":"<<view_snapshot_overflow_<<",\"table_descriptor_budgets\":[16,32,64],\"target_reads\":"<<targets_<<",\"candidate_rows\":"<<candidates_
            <<",\"target_without_predecessor\":"<<unmatched_<<",\"output_drops\":"<<output_drops_<<",\"descriptor_heap_write_generations\":\"unobserved_by_v2_source\"}";
        lines_.push_back(s.str());dirty_=false;summary_due_=now+5000;
    }
    out.files=std::move(files_);out.lines=std::move(lines_);files_.clear();lines_.clear();return out;
}
inline std::string NativeShapeEvidence::write(Batch batch){
    if(batch.directory.empty()||(batch.files.empty()&&batch.lines.empty()))return {};
    try{std::filesystem::create_directories(batch.directory);
        for(const auto &f:batch.files){const auto path=batch.directory/f.name;if(std::filesystem::exists(path))return "Shape evidence filename collision: "+f.name;
            std::ofstream out(path,std::ios::binary);if(!out)return "Shape evidence file open failed";
            if(f.code)out.write(reinterpret_cast<const char *>(f.code->bytes.data()),std::streamsize(f.code->bytes.size()));else out<<f.text;
            if(!out)return "Shape evidence container/layout write failed";
        }
        if(!batch.lines.empty()){std::ofstream out(batch.directory/"native-shape-evidence.jsonl",std::ios::app);for(const auto &s:batch.lines)out<<s<<'\n';if(!out)return "Shape evidence metadata write failed";}
    }catch(const std::exception &e){return std::string("Shape evidence IO: ")+e.what();}return {};
}
}
