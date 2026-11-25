#pragma once
#include "native_profile_registry.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
namespace hsin_rt {
// V2 is append-only, separate from the unchanged V1 seeds. Each complete
// newline-delimited record contains the full supported layout signature.
class NativeProfileArchive {
    std::filesystem::path path_;
    std::vector<std::shared_ptr<const NativeProfile>> stored_;
    std::mutex mutex_;
    std::uint64_t last_attempt_=UINT64_MAX;
    static inline std::mutex file_mutex_;
    static constexpr const char *header="WWMI_NATIVE_PROFILES_V2 float3-u32-v1";
public:
    explicit NativeProfileArchive(std::filesystem::path path):path_(std::move(path)){}
    std::string import(const std::shared_ptr<NativeProfileRegistry> &registry){
        stored_=registry->snapshot()->profiles;if(path_.empty())return {};
        std::lock_guard file(file_mutex_);std::ifstream in(path_,std::ios::binary);if(!in){if(std::filesystem::exists(path_))return "native profile archive cannot be read";return {};}
        std::string line;if(!std::getline(in,line)||line!=header)return "native profile archive V2 header mismatch";
        unsigned rejected=0;
        while(std::getline(in,line)){
            if(in.eof()){++rejected;break;} // torn trailing record is never authority
            std::istringstream row(line);std::string kind,tail;std::uint64_t guid=0,hash=0,n=0,count=0;
            NativeProfile profile;
            if(!(row>>kind>>guid>>hash>>n>>count)||kind!="mesh"||guid>UINT32_MAX||hash>UINT32_MAX||n>UINT32_MAX||!count||count>4096){++rejected;continue;}
            profile.object_guid=unsigned(guid);profile.content_hash=unsigned(hash);profile.vertices=unsigned(n);bool good=true;
            for(std::uint64_t i=0;i<count;++i){std::uint64_t id=0,first=0,length=0,index_first=0,index_count=0;
                if(!(row>>id>>first>>length>>index_first>>index_count)||id>UINT32_MAX||first>UINT32_MAX||length>UINT32_MAX||index_first>UINT32_MAX||index_count>UINT32_MAX){good=false;break;}
                profile.components.push_back({unsigned(id),unsigned(first),unsigned(length),unsigned(index_first),unsigned(index_count)});
            }
            if(!good||(row>>tail)){++rejected;continue;}
            try{stored_.push_back(registry->publish(profile).profile);}catch(const std::runtime_error &){++rejected;}
        }
        return rejected?"native profile archive rejected "+std::to_string(rejected)+" incomplete/invalid records":std::string{};
    }
    // Called only after Runtime/collector locks have been released at Present.
    std::string append(const NativeProfileRegistry::SnapshotRef &snapshot){
        if(path_.empty()||!snapshot)return {};
        std::lock_guard owner(mutex_);if(last_attempt_==snapshot->revision)return {};last_attempt_=snapshot->revision;std::vector<std::shared_ptr<const NativeProfile>> fresh;
        for(const auto &p:snapshot->profiles)if(!p->partial&&std::find(stored_.begin(),stored_.end(),p)==stored_.end())fresh.push_back(p);
        if(fresh.empty())return {};
        std::lock_guard file(file_mutex_);
        std::error_code error;const bool exists=std::filesystem::exists(path_,error);if(error)return "native profile archive existence check failed";
        bool separator=false;
        if(exists){std::ifstream in(path_,std::ios::binary);std::string first;if(!std::getline(in,first)||first!=header)return "native profile archive append refused: header mismatch";
            in.clear();in.seekg(-1,std::ios::end);char last=0;if(!in.get(last))return "native profile archive append refused: unreadable tail";separator=last!='\n';
        }
        std::ostringstream encoded;if(separator)encoded<<" INVALID_TORN_RECORD\n";if(!exists)encoded<<header<<'\n';
        for(const auto &p:fresh){encoded<<"mesh "<<p->object_guid<<' '<<p->content_hash<<' '<<p->vertices<<' '<<p->components.size();
            for(const auto &c:p->components)encoded<<' '<<c.component<<' '<<c.first<<' '<<c.count<<' '<<c.index_first<<' '<<c.index_count;encoded<<'\n';}
        std::ofstream out(path_,std::ios::binary|std::ios::app);const auto text=encoded.str();out.write(text.data(),std::streamsize(text.size()));out.flush();if(!out)return "native profile archive append failed";
        stored_.insert(stored_.end(),fresh.begin(),fresh.end());return {};
    }
};
}
