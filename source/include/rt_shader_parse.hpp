#pragma once
#include <cstdint>
#include <string_view>
// Parse only fields emitted by our native bridge; no GPU pointer is dereferenced.
inline bool rt_hex_field(std::string_view s, std::string_view key, uint64_t &out) noexcept {
    const auto p=s.find(key); if(p==s.npos) return false;
    auto i=p+key.size(); if(i+3>s.size() || s.substr(i,3)!="\"0x") return false; i+=3;
    uint64_t v=0; unsigned n=0;
    while(i<s.size() && s[i]!='"') {const char c=s[i++]; unsigned d;
        if(c>='0'&&c<='9') d=c-'0'; else if(c>='a'&&c<='f') d=c-'a'+10;
        else if(c>='A'&&c<='F') d=c-'A'+10; else return false;
        if(++n>16) return false;
        v=(v<<4)|d;
    }
    if(!n || i==s.size()) return false;
    out=v; return true;
}
inline bool rt_uint_field(std::string_view s, std::string_view key, uint32_t &out) noexcept {
    const auto p=s.find(key); if(p==s.npos) return false; auto i=p+key.size(); uint64_t v=0; unsigned n=0;
    while(i<s.size() && s[i]>='0' && s[i]<='9') {v=v*10+(s[i++]-'0'); if(v>0xffffffffull)return false; ++n;}
    if(!n || i==s.size() || (s[i]!=',' && s[i]!='}')) return false;
    out=(uint32_t)v; return true;
}
