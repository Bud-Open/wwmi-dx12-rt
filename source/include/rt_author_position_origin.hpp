#pragma once
#include <d3d12.h>
#include <string_view>

namespace wwmi::author_position_origin {

// Capture parser-owned static skin data at the actual CreateBuffer call.
// A writable exposure or later upload/copy revokes the byte proof below.
class InitialScope {
public:
    InitialScope(std::wstring_view resource_key,const void *initial,UINT bytes)
        : previous_(active_),initial_(initial),bytes_(bytes){
        const std::wstring_view resource=L"resource";
        const std::wstring_view scoped=L"resource\\";
        const bool is_scoped=resource_key.compare(0,scoped.size(),scoped)==0;
        const bool is_unscoped=resource_key.find(L'\\')==std::wstring_view::npos&&
            resource_key.compare(0,resource.size(),resource)==0;
        const auto name=is_scoped?resource_key.substr(resource_key.find_last_of(L'\\')+1):
            is_unscoped?resource_key.substr(resource.size()):std::wstring_view{};
        const bool static_skin=name==L"positionbuffer"||name==L"blendbuffer"||name==L"blendbuffernostride"||
            name==L"indexbuffer"||name==L"vectorbuffer"||name==L"texcoordbuffer"||name==L"colorbuffer"||
            name==L"shapekeyvertexidbuffer"||name==L"blendremapvertexvgbuffer"||
            name==L"blendremapforwardbuffer"||name==L"blendremapreversebuffer";
        active_=static_skin&&initial&&bytes?this:nullptr;
    }
    ~InitialScope(){active_=previous_;}
    InitialScope(const InitialScope &)=delete;
    InitialScope &operator=(const InitialScope &)=delete;
    static bool matches(const void *initial,UINT bytes){
        return active_&&active_->initial_==initial&&active_->bytes_==bytes;
    }
private:
    const InitialScope *previous_;
    const void *initial_;
    UINT bytes_;
    inline static thread_local const InitialScope *active_=nullptr;
};

// Attach the actual initial CPU bytes to this resource, not its VA, parser
// name or later filesystem contents. Set once; writable exposure or another
// upload/copy permanently revokes the evidence for this object.
inline constexpr GUID initial_position_id={0x1b35fcbc,0x71be,0x49d7,{0x93,0x6a,0x4e,0xcf,0x3c,0xc5,0x0b,0x4e}};
inline void set_initial(ID3D12Resource *resource,const void *bytes,UINT count){
    resource->SetPrivateData(initial_position_id,count,bytes);
}
inline void forget(ID3D12Resource *resource){
    resource->SetPrivateData(initial_position_id,0,nullptr);
}
inline bool still_initial(ID3D12Resource *resource,UINT bytes){
    UINT size=0;
    const HRESULT result=resource->GetPrivateData(initial_position_id,&size,nullptr);
    return (result==S_OK||result==DXGI_ERROR_MORE_DATA)&&size==bytes;
}
inline bool read_initial(ID3D12Resource *resource,void *out,UINT bytes){
    UINT size=bytes;
    return resource->GetPrivateData(initial_position_id,&size,out)==S_OK&&size==bytes;
}

}
