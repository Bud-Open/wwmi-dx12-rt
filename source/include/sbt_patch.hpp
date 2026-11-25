#pragma once
#include <array>
#include <cstdint>
#include <cstring>
namespace hsin_rt {
using HitRecord=std::array<uint32_t,32>;
using ShaderId=std::array<uint32_t,8>;
struct SbtRule {uint64_t old_ib,old_vb;uint32_t old_offset,old_stride;uint64_t new_ib,new_vb;uint32_t new_offset,new_stride;};
static_assert(sizeof(SbtRule)==48);
inline HitRecord patch_hit_record(const HitRecord &source,const ShaderId &shader,const SbtRule *rules,uint32_t count) {
    HitRecord out=source;
    if(std::memcmp(source.data(),shader.data(),32))return out;
    uint64_t ib=uint64_t(source[8])|(uint64_t(source[9])<<32),vb=uint64_t(source[10])|(uint64_t(source[11])<<32);
    for(uint32_t i=0;i<count;++i){const auto &r=rules[i];
        // The native BLAS fixes the first index address, not the SRV/base split.
        if(ib+source[13]!=r.old_ib+r.old_offset||vb!=r.old_vb||(source[12]&65535)!=r.old_stride)continue;
        out[8]=uint32_t(r.new_ib);out[9]=uint32_t(r.new_ib>>32);out[10]=uint32_t(r.new_vb);out[11]=uint32_t(r.new_vb>>32);
        out[12]=(source[12]&0xffff0000u)|r.new_stride;out[13]=r.new_offset;break;
    }
    return out;
}
}
