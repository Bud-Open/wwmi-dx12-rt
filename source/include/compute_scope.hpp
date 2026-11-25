#pragma once
#include <cstdint>
#include <vector>
namespace hsin_rt {
enum class RootKind : uint32_t { none,table,cbv,srv,uav,constants };
enum class PipelineKind : uint32_t { ordinary,ray_tracing };
struct ComputeSlot {
    RootKind kind=RootKind::none;
    uint64_t address=0,known_words=0;
    std::vector<uint32_t> words;
};
// Snapshot of the setters needed by this task. It does not identify resources
// or infer a value when the host failed to observe a setter.
class ComputeScope {
    uint64_t root_=0,pipeline_=0;
    PipelineKind pipeline_kind_=PipelineKind::ordinary;
    std::vector<ComputeSlot> slots_;
    bool observed_=true;
public:
    void reset(uint64_t initial_pipeline=0) {root_=0;pipeline_=initial_pipeline;pipeline_kind_=PipelineKind::ordinary;slots_.clear();observed_=true;}
    void invalidate(){observed_=false;}
    void signature(uint64_t root){if(root!=root_){root_=root;slots_.clear();observed_=true;}}
    void pipeline(PipelineKind kind,uint64_t value){pipeline_kind_=kind;pipeline_=value;}
    bool binding(RootKind kind,uint32_t index,uint64_t value){
        if(index>=64||!root_||kind==RootKind::none||kind==RootKind::constants){invalidate();return false;}
        if(slots_.size()<=index)slots_.resize(index+1);
        auto &s=slots_[index];s.kind=kind;s.address=value;s.known_words=0;s.words.clear();
        return true;
    }
    bool constants(uint32_t index,uint32_t first,uint32_t count,const uint32_t *data){
        if(index>=64||!root_||first>64||count>64-first||(!data&&count)){invalidate();return false;}
        if(!count)return true;
        if(slots_.size()<=index)slots_.resize(index+1);
        auto &s=slots_[index];if(s.kind!=RootKind::constants){s=ComputeSlot{};s.kind=RootKind::constants;}
        if(s.words.size()<first+count)s.words.resize(first+count);
        for(uint32_t i=0;i<count;++i){s.words[first+i]=data[i];s.known_words|=uint64_t(1)<<(first+i);}
        return true;
    }
    void clear_tables(){for(auto &s:slots_)if(s.kind==RootKind::table)s=ComputeSlot{};}
    uint64_t root_value()const{return root_;}
    uint64_t pipeline_value()const{return pipeline_;}
    PipelineKind pipeline_kind()const{return pipeline_kind_;}
    bool ready()const{return observed_&&root_&&pipeline_;}
    // The restore reader checks its own, narrower proof before visiting.
    template<class Sink> void visit_arguments(Sink &sink,bool tables_only=false)const {
        for(uint32_t i=0;i<slots_.size();++i){const auto &s=slots_[i];
            if(tables_only&&s.kind!=RootKind::table)continue;
            if(s.kind==RootKind::constants){
                for(uint32_t word=0;word<s.words.size();){
                    if(!(s.known_words&(uint64_t(1)<<word))){++word;continue;}
                    uint32_t first=word++;
                    while(word<s.words.size()&&(s.known_words&(uint64_t(1)<<word)))++word;
                    sink.constants(i,first,word-first,s.words.data()+first);
                }
            }else if(s.kind!=RootKind::none)sink.binding(s.kind,i,s.address);
        }
    }
    template<class Sink> bool apply(Sink &sink)const {
        if(!ready())return false;
        sink.signature(root_);
        visit_arguments(sink);
        sink.pipeline(pipeline_kind_,pipeline_);return true;
    }
};
}
