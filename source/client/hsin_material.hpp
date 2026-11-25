#pragma once
#include "hsin_inputs.hpp" // the same candidate-local collector as hsin_runtime.cpp
#include "sbt_patch.hpp"
#include <unordered_set>
#include <cstring>
namespace hsin_rt {
// Uses the existing SDK library graph, plus exact known shader bytes. No new native trace subscription.
class MaterialIdentity {
    std::vector<unsigned char> code_;
    std::unordered_set<std::uint64_t> proven_;
public:
    MaterialIdentity()=default;
    explicit MaterialIdentity(std::span<const unsigned char> code):code_(code.begin(),code.end()){}
    void init(api::pipeline pipeline,unsigned count,const api::pipeline_subobject *subobjects){
        bool chs=false,ahs=false,inherited=false;
        for(unsigned i=0;i<count;++i){const auto &s=subobjects[i];
            if(s.type==api::pipeline_subobject_type::libraries){
                auto *parents=static_cast<const api::pipeline *>(s.data);
                for(unsigned j=0;j<s.count;++j)inherited|=proven_.contains(parents[j].handle);
            }
            if(s.type!=api::pipeline_subobject_type::closest_hit_shader&&s.type!=api::pipeline_subobject_type::any_hit_shader)continue;
            auto *shaders=static_cast<const api::shader_desc *>(s.data);
            for(unsigned j=0;j<s.count;++j){const auto &shader=shaders[j];
                if(!shader.entry_point||shader.code_size!=code_.size()||code_.empty()||memcmp(shader.code,code_.data(),code_.size()))continue;
                // The SDK reports RDAT's original unmangled function, not
                // the D3D12 export alias. Match that source against the exact
                // bytes here; identifier() verifies the live alias separately.
                chs|=strcmp(shader.entry_point,"LumenHardwareRayTracingMaterialCHS")==0;
                ahs|=strcmp(shader.entry_point,"LumenHardwareRayTracingMaterialAHS")==0;
            }
        }
        if((chs&&ahs)||inherited)proven_.insert(pipeline.handle);else proven_.erase(pipeline.handle);
    }
    void destroy(api::pipeline pipeline){proven_.erase(pipeline.handle);}
    bool identifier(std::uint64_t state_object,ShaderId &id) const {
        if(!proven_.contains(state_object))return false;
        ComPtr<ID3D12StateObjectProperties> properties;
        if(FAILED(reinterpret_cast<ID3D12StateObject *>(state_object)->QueryInterface(IID_PPV_ARGS(&properties))))return false;
        const void *data=properties->GetShaderIdentifier(L"HitGroup_812aa0045dfa6e70");
        if(!data)return false;memcpy(id.data(),data,32);return true;
    }
};
}
