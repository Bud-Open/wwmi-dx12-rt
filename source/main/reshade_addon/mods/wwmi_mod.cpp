// ===========================================================================
//  wwmi_mod.cpp -- adopting a mod's buffers from their engine, plus two accessors.
//
//  This file reads no .ini files. The mod's component layout, its resources and its object
//  constants all come from THEIR parsed engine; wwmi_mod_engine.cpp is the whole reader.
//
//  It creates no resources either: LoadResources ADOPTS the DX12 buffer their
//  CustomResource::Substantiate already made from the same file, so there is one loader for a mod's
//  bytes, not two.
// ===========================================================================
#include "wwmi_mod.h"
#include "../migoto/port_api.h" // GetBufferResource -- their object, not a second copy
#include "../../src/log.h"
#include <cwctype>
#include <cstring>
#include <string>

namespace WwmiMod {

static inline wchar_t fast_towlower(wchar_t c) noexcept
{
    if (c >= L'A' && c <= L'Z') return static_cast<wchar_t>(c + (L'a' - L'A'));
    if (static_cast<unsigned int>(c) < 0x80u) return c;
    return static_cast<wchar_t>(towlower(c));
}

static inline bool ieq_wide(const std::wstring& a, const wchar_t* b) noexcept
{
    if (!b) return false;
    const size_t   n = a.size();
    const wchar_t* p = a.data();
    for (size_t i = 0; i < n; ++i) {
        const wchar_t bc = b[i];
        if (bc == L'\0' || fast_towlower(p[i]) != fast_towlower(bc)) return false;
    }
    return b[n] == L'\0';
}

static inline bool has_buf_ext(const std::wstring& fn) noexcept
{
    if (fn.size() < 4) return false;
    const wchar_t* p = fn.data() + (fn.size() - 4);
    return p[0] == L'.' && fast_towlower(p[1]) == L'b' && fast_towlower(p[2]) == L'u' && fast_towlower(p[3]) == L'f';
}

int LoadResources(Mod& m, ID3D12Device* /*dev*/)
{
    int n = 0;
    int missing = 0;
    for (auto& r : m.resources) {
        r.res = nullptr;
        r.gpu_va = 0;
        r.size = 0;
        if (r.fq.empty()) continue;
        PortApi::BufferResourceInfo bi;
        if (!PortApi::GetBufferResource(r.fq.c_str(), &bi) || !bi.dx12) {
            if (has_buf_ext(r.filename)) ++missing;
            continue;
        }
        r.res = bi.dx12;
        r.gpu_va = bi.gpu_va;
        r.size = bi.byte_width;
        r.stride = bi.stride;
        ++n;
    }
    logger::write("WwmiMod: adopted %d buffer(s) from their engine%s", n,
        missing ? " (some file resources had no DX12 buffer -- see below)" : "");
    if (missing)
        for (const auto& r : m.resources)
            if (!r.res && has_buf_ext(r.filename))
                logger::write("  no DX12 buffer for %ls ('%ls')", r.name.c_str(), r.filename.c_str());

    // Reconcile mesh_vertex_count against ResourcePositionBuffer when the author
    // edited vertices without updating [Constants] $mesh_vertex_count, provided
    // Position and Blend buffers agree on the actual vertex count.
    const Resource* pos = Find(m, L"ResourcePositionBuffer");
    const Resource* blend = Find(m, L"ResourceBlendBuffer");
    if (pos && pos->size > 0 && pos->size % 12u == 0 && (!pos->stride || pos->stride == 12u)) {
        const uint32_t pos_verts = pos->size / 12u;
        if (pos_verts > 0 && pos_verts != m.mesh_vertex_count) {
            const bool blend_ok = !blend || !blend->size ||
                (blend->stride
                        ? (uint64_t(blend->stride) * pos_verts == blend->size)
                        : (blend->size == 8ull * pos_verts || blend->size == 16ull * pos_verts ||
                              blend->size == 32ull * pos_verts));
            if (blend_ok) {
                logger::write("WwmiMod: reconciled mesh_vertex_count %u -> %u from ResourcePositionBuffer (%u bytes)",
                    m.mesh_vertex_count, pos_verts, pos->size);
                m.mesh_vertex_count = pos_verts;
            }
        }
    }
    return n;
}

const Resource* Find(const Mod& m, const wchar_t* name)
{
    if (!name) return nullptr;
    for (const auto& r : m.resources)
        if (ieq_wide(r.name, name)) return &r;
    return nullptr;
}

void LogSelfCheck(const Mod& m)
{
    logger::write("WwmiMod self-check: %zu components, %zu resources, fwdmap='%ls'",
        m.comps.size(), m.resources.size(), m.forward_map_file.c_str());
    for (const auto& c : m.comps) {
        int ndraws = (int)c.draws.size();
        if (c.remapped)
            logger::write("  comp%d first=%u count=%u vg_off=%u vg_cnt=%u REMAPPED id=%d remap_vg=%u draws=%d",
                c.index, c.match_first_index, c.match_index_count, c.vg_offset, c.vg_count, c.remap_id, c.remap_vg, ndraws);
        else
            logger::write("  comp%d first=%u count=%u vg_off=%u vg_cnt=%u direct draws=%d",
                c.index, c.match_first_index, c.match_index_count, c.vg_offset, c.vg_count, ndraws);
        for (const auto& d : c.draws)
            logger::write("      draw count=%u start=%u%s%ls", d.count, d.start,
                d.cond.empty() ? "" : "  if ", d.cond.c_str());
    }
    int nfile = 0;
    for (const auto& r : m.resources)
        if (!r.filename.empty()) ++nfile;
    logger::write("  (%d resources have filenames)", nfile);
    const wchar_t* key[] = { L"ResourcePositionBuffer", L"ResourceBlendBuffer", L"ResourceVectorBuffer",
        L"ResourceTexcoordBuffer", L"ResourceColorBuffer", L"ResourceIndexBuffer" };
    for (const wchar_t* nm : key) {
        const Resource* r = Find(m, nm);
        if (r) logger::write("  %ls: file='%ls' size=%u stride=%u va=%llx%s", nm, r->filename.c_str(),
            r->size, r->stride, r->gpu_va, r->res ? "" : " (NOT LOADED)");
    }
}

} // namespace WwmiMod
