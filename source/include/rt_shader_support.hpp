#pragma once
#include <windows.h>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include "rt_shader_parse.hpp"
inline bool rt_cpu_copy(const void *src, size_t bytes, std::vector<uint8_t> &out) {
    if(!src || !bytes || bytes>2u*1024u*1024u) return false;
    out.resize(bytes); SIZE_T got=0;
    return ReadProcessMemory(GetCurrentProcess(),src,out.data(),bytes,&got)!=FALSE && got==bytes;
}
inline bool rt_write_new(const std::wstring &path,const std::vector<uint8_t> &data) noexcept {
    HANDLE f=CreateFileW(path.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE) return false;
    DWORD put=0; const bool written=WriteFile(f,data.data(),(DWORD)data.size(),&put,nullptr)!=FALSE && put==data.size();
    return CloseHandle(f)!=FALSE && written;
}
