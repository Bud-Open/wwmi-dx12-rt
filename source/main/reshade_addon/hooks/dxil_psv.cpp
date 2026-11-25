// dxil_psv.cpp -- see dxil_psv.h.
//
// Container: 'DXBC', 16-byte digest, u32 version, u32 total size, u32 part count, u32 offsets[count];
// each part: fourcc, u32 size, data. PSV0 (DxilPipelineStateValidation.h, dxc):
//   u32 PSVRuntimeInfo_size; PSVRuntimeInfo (that many bytes; its version is implied by the size);
//   u32 ResourceCount; if ResourceCount: u32 PSVResourceBindInfo_size; ResourceCount records of that size:
//     PSVResourceBindInfo0 { u32 ResType; u32 Space; u32 LowerBound; u32 UpperBound; }        (16 bytes)
//     PSVResourceBindInfo1 adds { u32 ResKind; u32 ResFlags; }                                   (24 bytes)
//   (string table, semantic index table and signature elements follow in v1+; not needed.)
// ISG1 / OSG1 (DxilProgramSignature): u32 ElementCount; u32 ElementOffset; then 32-byte elements from that
//   offset: { u32 Stream; u32 SemanticName (offset of a NUL-terminated string in the part); u32 SemanticIndex;
//   u32 SystemValue; u32 CompType; u32 Register; u8 Mask; u8 UsageMask; u16; u32 MinPrecision; }.
// PSVResourceType: 0 Invalid, 1 Sampler, 2 CBV, 3 SRVTyped, 4 SRVRaw, 5 SRVStructured, 6..9 UAVs.
// UpperBound is 0xFFFFFFFF for an unbounded range. Everything is read through bounds-checked u32 loads:
// a truncated or foreign blob yields `parsed = false`, never a read past the end.
#include "dxil_psv.h"
#include "rt_raster_effects.h"
#include <d3d12shader.h>
#include <cstring>

namespace {
bool u32_at(const uint8_t* p, size_t size, size_t off, uint32_t* out)
{
    if (off + 4 > size) return false;
    memcpy(out, p + off, 4);
    return true;
}

bool SignatureNames(const uint8_t* d, size_t n, std::vector<std::string>& out)
{
    uint32_t count = 0;
    uint32_t first = 0;
    if (!u32_at(d, n, 0, &count) || !u32_at(d, n, 4, &first) || count > 64) return false;
    for (uint32_t e = 0; e < count; ++e) {
        const size_t at = (size_t)first + (size_t)e * 32;
        uint32_t     name = 0;
        uint32_t     index = 0;
        if (!u32_at(d, n, at + 4, &name) || !u32_at(d, n, at + 8, &index) || name >= n) return false;
        const char*  s = reinterpret_cast<const char*>(d + name);
        const size_t len = strnlen(s, n - name);
        if (len == n - name) return false;
        out.push_back(std::string(s, len) + std::to_string(index));
    }
    return true;
}
}

static bool DxilShaderBindingRegs(const void* code, size_t size, DxilSrvRegs& out, bool uav, bool strict=false)
{
    out = DxilSrvRegs{};
    const uint8_t* p = static_cast<const uint8_t*>(code);
    if (!p || size < 32 || memcmp(p, "DXBC", 4) != 0) return false;
    uint32_t nparts = 0;
    if (!u32_at(p, size, 28, &nparts) || nparts > 64) return false;
    for (uint32_t i = 0; i < nparts; ++i) {
        uint32_t off = 0;
        uint32_t psize = 0;
        if (!u32_at(p, size, 32 + (size_t)i * 4, &off)) return false;
        if (off + 8 > size || memcmp(p + off, "PSV0", 4) != 0) continue;
        if (!u32_at(p, size, off + 4, &psize) || off + 8 + (size_t)psize > size) return false;
        const uint8_t* d = p + off + 8;
        const size_t   n = psize;
        uint32_t       info_size = 0;
        uint32_t       res_count = 0;
        uint32_t       rec_size = 0;
        size_t         pos = 0;
        if (!u32_at(d, n, pos, &info_size)) return false;
        pos += 4 + info_size;
        if (!u32_at(d, n, pos, &res_count)) return false;
        pos += 4;
        if (res_count == 0) {
            out.parsed = true;
            return true;
        }
        if (!u32_at(d, n, pos, &rec_size) || rec_size < 16 || res_count > 4096) return false;
        pos += 4;
        if (pos + (size_t)res_count * rec_size > n) return false;
        for (uint32_t r = 0; r < res_count; ++r, pos += rec_size) {
            uint32_t type = 0;
            uint32_t space = 0;
            uint32_t lo = 0;
            uint32_t hi = 0;
            uint32_t kind = 0;
            u32_at(d, n, pos + 0, &type);
            u32_at(d, n, pos + 4, &space);
            u32_at(d, n, pos + 8, &lo);
            u32_at(d, n, pos + 12, &hi);
            if (rec_size >= 24) u32_at(d, n, pos + 16, &kind);
            if (uav ? (type < 6 || type > 9) : (type < 3 || type > 5)) continue;
            if (strict && (space || hi < lo || hi >= 128)) return false;
            if (space != 0) continue;
            if (hi == 0xFFFFFFFFu || hi < lo) {
                out.unbounded = true;
                continue;
            }
            for (uint32_t t = lo; t <= hi && t < 128; ++t) {
                if (strict && out.has(t)) return false;
                if (t < 64)
                    out.lo |= (1ull << t);
                else
                    out.hi |= (1ull << (t - 64));
                out.kind[t] = (uint8_t)(kind > 255 ? 0 : kind);
            }
        }
        out.parsed = true;
        return true;
    }
    return false;
}

bool DxilShaderSrvRegs(const void* code,size_t size,DxilSrvRegs& out){return DxilShaderBindingRegs(code,size,out,false);}
bool RtStrictPixelSrvRegs(const void* code,size_t size,DxilSrvRegs& out){return DxilShaderBindingRegs(code,size,out,false,true);}
bool RtShaderUavRegs(const void* code,size_t size,DxilSrvRegs& out){return DxilShaderBindingRegs(code,size,out,true);}

namespace {
// All register spaces, including unbounded ranges. This is evidence of
// possible UAV effects, not of a store having happened.
bool raster_bindings(const uint8_t *p, size_t n, bool &uav)
{
    uint32_t runtime, count, stride;
    if (!u32_at(p,n,0,&runtime) || runtime > n-4) return false;
    const size_t at = 4 + size_t(runtime);
    if (!u32_at(p,n,at,&count)) return false;
    if (!count) return true;
    if (!u32_at(p,n,at+4,&stride) || stride < 16 || count > (n-at-8)/stride) return false;
    for (uint32_t i=0; i<count; ++i) {
        uint32_t type;
        u32_at(p,n,at+8+size_t(i)*stride,&type);
        if (type < 1 || type > 9) return false;
        uav |= type >= 6;
    }
    return true;
}

bool raster_outputs(const uint8_t *p, size_t n, RtRasterShaderFacts &out)
{
    uint32_t count, first;
    if (!u32_at(p,n,0,&count) || !u32_at(p,n,4,&first) ||
        first < 8 || first > n || count > (n-first)/32) return false;
    for (uint32_t i=0; i<count; ++i) {
        const auto at = first+size_t(i)*32;
        uint32_t stream, name, index, semantic;
        u32_at(p,n,at,&stream); u32_at(p,n,at+4,&name);
        u32_at(p,n,at+8,&index); u32_at(p,n,at+12,&semantic);
        if (stream || name >= n || !memchr(p+name,0,n-name)) return false;
        const char *text = reinterpret_cast<const char *>(p+name);
        // Some compiler paths leave SystemValue zero in a DXIL signature.
        // The name and separate SemanticIndex are the authoritative pair in
        // that case. Output UsageMask is NOT the declared channel mask.
        if (semantic==D3D_NAME_TARGET || !_stricmp(text,"SV_Target")) {
            const auto channels=uint8_t(p[at+24]&0x0f);
            if (index>=8 || !channels) return false;
            out.color_slots|=uint8_t(1u<<index);
            out.color_channels[index]|=channels;
        } else if (semantic==D3D_NAME_DEPTH || semantic==D3D_NAME_DEPTH_GREATER_EQUAL ||
                   semantic==D3D_NAME_DEPTH_LESS_EQUAL || !_stricmp(text,"SV_Depth") ||
                   !_stricmp(text,"SV_DepthGreaterEqual") || !_stricmp(text,"SV_DepthLessEqual")) {
            out.depth=true;
        } else if (semantic==D3D_NAME_COVERAGE || !_stricmp(text,"SV_Coverage")) {
            out.coverage=true;
        } else if (semantic==D3D_NAME_STENCIL_REF || !_stricmp(text,"SV_StencilRef")) {
            out.stencil_ref=true;
        } else {
            // A foreign/unsupported pixel output is not an empty signature.
            return false;
        }
    }
    return true;
}
}

void RtReadRasterShaderFacts(const void *code, size_t size, bool pixel, RtRasterShaderFacts &out)
{
    out={};
    const auto *p=static_cast<const uint8_t *>(code);
    uint32_t container,parts;
    if (!p || size<32 || memcmp(p,"DXBC",4) ||
        !u32_at(p,size,24,&container) || container!=size ||
        !u32_at(p,size,28,&parts) || parts>64 || parts>(size-32)/4) return;
    RtRasterShaderFacts result;
    bool psv_seen=false, psv_valid=false, uav=false, feature_seen=false, direct=false;
    bool sig_seen=false, sig_valid=false, input_seen=false, input_valid=false, program=false;
    for (uint32_t i=0; i<parts; ++i) {
        uint32_t off,n;
        u32_at(p,size,32+4ull*i,&off);
        if (off<32+4ull*parts || off>size || size-off<8 ||
            !u32_at(p,size,size_t(off)+4,&n) || n>size-off-8) return;
        const auto *part=p+off;
        if (!memcmp(part,"DXIL",4)) {
            if (program || n<24) return;
            // ProgramVersion high word is the DXIL ShaderKind (Pixel=0).
            uint32_t version;
            u32_at(part+8,n,0,&version);
            if (pixel && (version>>16)!=0) return;
            program=true;
        } else if (!memcmp(part,"PSV0",4)) {
            if (psv_seen) return;
            psv_seen=true; psv_valid=raster_bindings(part+8,n,uav);
        } else if (!memcmp(part,"SFI0",4)) {
            if (feature_seen || n!=8) return;
            uint64_t flags;
            memcpy(&flags,part+8,sizeof(flags));
            feature_seen=true;direct=(flags&D3D_SHADER_REQUIRES_RESOURCE_DESCRIPTOR_HEAP_INDEXING)!=0;
        } else if (pixel && !memcmp(part,"OSG1",4)) {
            if (sig_seen) return;
            sig_seen=true;sig_valid=raster_outputs(part+8,n,result);
        } else if (pixel && !memcmp(part,"ISG1",4)) {
            if (input_seen) return;
            input_seen=true;
            std::vector<std::string> inputs;
            input_valid=SignatureNames(part+8,n,inputs);
        }
    }
    if (!program) return;
    result.outputs_known=pixel&&sig_valid;
    result.standard_shape=pixel&&sig_valid&&psv_valid&&input_valid;
    if ((psv_valid&&uav)||direct) result.uav=RtEffect::Possible;
    else if (psv_valid&&feature_seen) result.uav=RtEffect::Absent;
    out=result;
}

bool DxilShaderShape(const void* code, size_t size, DxilShape& out)
{
    out = DxilShape{};
    const uint8_t* p = static_cast<const uint8_t*>(code);
    if (!p || size < 32 || memcmp(p, "DXBC", 4) != 0) return false;
    uint32_t nparts = 0;
    if (!u32_at(p, size, 28, &nparts) || nparts > 64) return false;
    bool psv = false;
    bool isg = false;
    bool osg = false;
    for (uint32_t i = 0; i < nparts; ++i) {
        uint32_t off = 0;
        uint32_t psize = 0;
        if (!u32_at(p, size, 32 + (size_t)i * 4, &off) || off + 8 > size) return false;
        if (!u32_at(p, size, off + 4, &psize) || off + 8 + (size_t)psize > size) return false;
        const uint8_t* d = p + off + 8;
        const size_t   n = psize;
        if (memcmp(p + off, "ISG1", 4) == 0)
            isg = SignatureNames(d, n, out.inputs);
        else if (memcmp(p + off, "OSG1", 4) == 0)
            osg = SignatureNames(d, n, out.outputs);
        else if (memcmp(p + off, "PSV0", 4) == 0) {
            uint32_t info_size = 0;
            uint32_t res_count = 0;
            uint32_t rec_size = 0;
            size_t   pos = 0;
            if (!u32_at(d, n, pos, &info_size)) return false;
            pos += 4 + info_size;
            if (!u32_at(d, n, pos, &res_count)) return false;
            pos += 4;
            if (res_count) {
                if (!u32_at(d, n, pos, &rec_size) || rec_size < 16 || res_count > 4096) return false;
                pos += 4;
                if (pos + (size_t)res_count * rec_size > n) return false;
            }
            for (uint32_t r = 0; r < res_count; ++r, pos += rec_size) {
                uint32_t type = 0;
                u32_at(d, n, pos, &type);
                if (type == 1)
                    ++out.samplers;
                else if (type == 2)
                    ++out.cbuffers;
                else if (type == 3)
                    ++out.typed_srvs;
                else
                    ++out.other_resources;
            }
            psv = true;
        }
    }
    return psv && isg && osg;
}
