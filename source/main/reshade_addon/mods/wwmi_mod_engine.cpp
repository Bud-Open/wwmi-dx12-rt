// ===========================================================================
//  wwmi_mod_engine.cpp -- the mod's layout, READ FROM THE PORTED ENGINE.
//
//  This file is the WHOLE reader: no second parser re-reads the .ini files their IniHandler has
//  already read.
//
//  Every value below is READ, never derived:
//     index / hash / match_first_index / match_index_count   [TextureOverrideComponentN] keys, and those of
//                                                            any other section matching an IB's index range
//     vg_offset / vg_count                                   $\WWMIv1\vg_offset, $\WWMIv1\vg_count
//     drawindexed sub-draws                                  the section's lines, IN FILE ORDER,
//                                                            including those reached by `run =`
//     remapped / remap_id / remap_vg                         [CommandListRemapMergedSkeleton]
//     resources + their filenames                            their [Resource*] store, ABSOLUTE paths
//     object_guid / mesh_vertex_count                        [Constants] globals, via their var store
//     the mod folder                                         their ini_path bookkeeping, not the ns
//
//  NAME SHAPES ARE NOT GUESSED. They were dumped from the live parse with `parse_test.exe -dumpns`
//  (writes port_sections.txt):
//     TextureOverride\SomeMod\Component0              <- author case, ResolveSection finds it
//     resource\somemod\blendbuffer  <abs filename>    <- lowercased
//     customshader\mods\somemod\mod.ini\transparency_1
//  ResolveSection indexes TextureOverride* and CommandList* only, so a [CustomShader*] is reached by
//  CONSTRUCTING `CustomShader\<ns>\<tail>`. That is proven, not assumed: the same harness reports
//  "customshader run-sites: 3 referenced, 3 reachable by constructed name, 3 drawindexed inside them".
// ===========================================================================
#include "wwmi_mod.h"
#include "../migoto/port_api.h"
#include "../../src/log.h"

#include <algorithm>
#include <cstdio> // swscanf_s -- drawindexed parsing
#include <cwctype>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace WwmiMod {

namespace {

std::wstring Lower(std::wstring s)
{
    for (auto& c : s) c = (wchar_t)towlower(c);
    return s;
}

bool ParseDrawIndexed(const std::wstring& val, SubDraw& out)
{
    if (Lower(val).rfind(L"auto", 0) == 0) return false; // handled by the draw hook, not a fixed range
    unsigned c = 0;
    unsigned s = 0;
    if (swscanf_s(val.c_str(), L" %u , %u", &c, &s) < 2) return false;
    out.count = c;
    out.start = s;
    return c != 0;
}

std::wstring NsOfFq(const std::wstring& fq)
{
    const size_t first = fq.find(L'\\');
    const size_t last = fq.find_last_of(L'\\');
    if (first == std::wstring::npos || last == std::wstring::npos || first >= last) return std::wstring();
    return fq.substr(first + 1, last - first - 1);
}

std::wstring TailOfFq(const std::wstring& fq)
{
    const size_t last = fq.find_last_of(L'\\');
    return last == std::wstring::npos ? fq : fq.substr(last + 1);
}

const wchar_t* AfterPsSlot(const wchar_t* p)
{
    if (_wcsnicmp(p, L"ps-t", 4) != 0) return nullptr;
    const wchar_t* q = p + 4;
    if (*q == L'[') {
        while (*q && *q != L']') ++q;
        return *q ? q + 1 : nullptr;
    }
    if (!iswdigit(*q)) return nullptr;
    while (iswdigit(*q)) ++q;
    return q;
}

bool NamesPsSlot(const wchar_t* s, bool metadata_only)
{
    if (!s) return false;
    for (const wchar_t* p = s; *p; ++p) {
        const wchar_t* q = AfterPsSlot(p);
        if (!q) continue;
        if (!metadata_only) return true;
        while (*q == L' ') ++q;
        if (q[0] == L'-' && q[1] == L'>') return true;
    }
    return false;
}

bool IsIniParam(const wchar_t* k)
{
    if (!k || !*k || !wcschr(L"xyzwXYZW", *k)) return false;
    for (const wchar_t* p = k + 1; *p; ++p)
        if (!iswdigit(*p)) return false;
    return true;
}

bool QueriesPsSlot(const wchar_t* line)
{
    if (!line) return false;
    for (const wchar_t* p = line; *p; ++p) {
        if (!_wcsnicmp(p, L"ps->srv_", 8) || !_wcsnicmp(p, L"IterateTextureSlots", 19)) return true;
        if (AfterPsSlot(p) && p[4] == L'[') return true;
    }
    while (*line == L' ' || *line == L'\t') ++line;
    if (!_wcsnicmp(line, L"if ", 3) || !_wcsnicmp(line, L"elif ", 5) || !_wcsnicmp(line, L"else if ", 8))
        return NamesPsSlot(line, false);
    const wchar_t* eq = wcschr(line, L'=');
    if (!eq) return NamesPsSlot(line, true);
    const wchar_t* k = line;
    for (;;) {
        if (!_wcsnicmp(k, L"pre ", 4)) k += 4;
        else if (!_wcsnicmp(k, L"post ", 5)) k += 5;
        else if (!_wcsnicmp(k, L"local ", 6)) k += 6;
        else break;
        while (*k == L' ') ++k;
    }
    std::wstring target(k, eq);
    while (!target.empty() && (target.back() == L' ' || target.back() == L'\t')) target.pop_back();
    if ((!target.empty() && target[0] == L'$') || IsIniParam(target.c_str())) return NamesPsSlot(eq + 1, false);
    return NamesPsSlot(line, true);
}

struct KeyCtx {
    Component*            comp;
    std::vector<SubDraw>* draws;
    std::wstring          ns;           // author-case namespace, for constructing CustomShader names
    std::wstring          pending_cond; // `if <expr>` currently open; recorded for the log only
    int                   depth;        // run-expansion depth guard
};

void OnKey(const wchar_t* key, const wchar_t* val, void* user);

void ExpandRun(KeyCtx* k, const std::wstring& target)
{
    if (k->depth >= 4) return; // pathological self-reference guard
    if (_wcsnicmp(target.c_str(), L"CustomShader", 12) != 0) return;
    const std::wstring tail = target.substr(12);
    if (tail.empty() || tail[0] == L'\\') return; // namespaced framework shader, not a part

    const std::wstring fq = L"CustomShader\\" + k->ns + L"\\" + tail;
    KeyCtx             sub{ k->comp, k->draws, k->ns, k->pending_cond, k->depth + 1 };
    PortApi::ForEachSectionKey(fq.c_str(), &OnKey, &sub);
}

void OnKey(const wchar_t* key, const wchar_t* val, void* user)
{
    KeyCtx*            k = (KeyCtx*)user;
    const std::wstring lk = Lower(key ? key : L"");
    const std::wstring v = val ? val : L"";

    if (lk == L"hash") {
        k->comp->ib_hash = (uint32_t)wcstoul(v.c_str(), nullptr, 16);
        return;
    }
    if (lk == L"match_first_index") {
        k->comp->match_first_index = (uint32_t)wcstoul(v.c_str(), nullptr, 10);
        return;
    }
    if (lk == L"match_index_count") {
        k->comp->match_index_count = (uint32_t)wcstoul(v.c_str(), nullptr, 10);
        return;
    }
    if (lk == L"match_priority") {
        k->comp->match_priority = (int)wcstol(v.c_str(), nullptr, 10);
        return;
    }

    // WWMI passes the merge window as namespaced variables rather than section keys.
    if (lk.find(L"vg_offset") != std::wstring::npos) {
        k->comp->vg_offset = (uint32_t)wcstoul(v.c_str(), nullptr, 10);
        return;
    }
    if (lk.find(L"vg_count") != std::wstring::npos) {
        k->comp->vg_count = (uint32_t)wcstoul(v.c_str(), nullptr, 10);
        return;
    }

    if (lk == L"if") {
        k->pending_cond = v;
        return;
    }
    if (lk == L"endif") {
        k->pending_cond.clear();
        return;
    }

    if (lk == L"run") {
        ExpandRun(k, v);
        return;
    }

    if (lk == L"drawindexed") {
        SubDraw d{};
        if (ParseDrawIndexed(v, d)) {
            d.cond = k->pending_cond;
            k->draws->push_back(d);
        }
        return;
    }
}

struct RemapInfo {
    int      comp_idx;
    int      id;
    uint32_t vg;
};

struct RemapCtx {
    std::vector<RemapInfo>* out;
    int                     rm_id = -1;
    uint32_t                rm_vg = 0;
};

void OnRemapKey(const wchar_t* key, const wchar_t* val, void* user)
{
    RemapCtx*                 r = (RemapCtx*)user;
    const std::wstring        lk = Lower(key ? key : L"");
    const std::wstring        v = val ? val : L"";
    static const std::wstring kPrimary = L"resourceremappedskeletoncomponent";

    if (lk == L"$\\wwmiv1\\blend_remap_id") {
        r->rm_id = _wtoi(v.c_str());
        return;
    }
    if (lk == L"$\\wwmiv1\\vg_count") {
        r->rm_vg = (uint32_t)wcstoul(v.c_str(), nullptr, 10);
        return;
    }

    if (lk.rfind(kPrimary, 0) == 0) {
        // "ResourceRemappedSkeletonComponentN = copy ResourceRemappedSkeletonRW" -- PRIMARY remap output
        if (Lower(v).rfind(L"copy", 0) == 0)
            r->out->push_back({ _wtoi(lk.c_str() + kPrimary.size()), r->rm_id, r->rm_vg });
        return;
    }
}

struct ResCtx {
    const std::wstring*    ns_lower;
    std::vector<Resource>* out;
};

void OnResource(const wchar_t* fq_name, const wchar_t* filename, int /*type*/, bool /*substantiated*/,
    ID3D12Resource* /*dx12*/, void*                    user)
{
    ResCtx* c = (ResCtx*)user;
    if (!fq_name) return;
    const std::wstring fq = fq_name;
    if (Lower(NsOfFq(fq)) != *c->ns_lower) return;

    const size_t last = fq.find_last_of(L'\\');
    if (last == std::wstring::npos) return;

    Resource r;
    r.name = L"Resource" + fq.substr(last + 1);
    r.fq = fq; // their key -- how the buffer is asked for later
    r.filename = filename ? filename : L"";
    c->out->push_back(std::move(r));
}

} // namespace

// Read one component section into `out`. `fq_section` is their registered name.
static bool ReadComponent(const std::wstring& fq_section, int index, Component& out)
{
    out = Component{};
    out.index = index;
    out.tail = TailOfFq(fq_section);
    KeyCtx ctx{ &out, &out.draws, NsOfFq(fq_section), std::wstring(), 0 };
    PortApi::ForEachSectionKey(fq_section.c_str(), &OnKey, &ctx);
    return true;
}

// The whole mod layout for a namespace, straight from the engine. Returns the number of components read.
int ReadComponentsFromEngine(const std::wstring& ns, Mod& out)
{
    out = Mod{};

    std::wstring ns_fq; // the namespace exactly as their parser spells it, taken from a real section
    {
        wchar_t probe[512] = {};
        if (PortApi::ResolveSection(ns.c_str(), nullptr, L"Component0", probe, std::size(probe)) && probe[0]) {
            ns_fq = NsOfFq(probe);
            wchar_t folder[512] = {};
            if (PortApi::SectionFolder(probe, folder, std::size(folder))) out.dir = folder;
        }
    }
    if (ns_fq.empty()) ns_fq = ns;

    // [Constants] globals, from their variable store rather than a re-read of the file.
    out.object_guid = (int)PortApi::GetVarNs(ns.c_str(), L"object_guid", 0.f);
    out.mesh_vertex_count = (uint32_t)PortApi::GetVarNs(ns.c_str(), L"mesh_vertex_count", 0.f);

    std::unordered_map<std::wstring, std::wstring> tail_to_fq;

    // Components are numbered from 0 and dense in every shipped WWMI mod: the first gap ends the scan.
    for (int i = 0;; ++i) {
        wchar_t            fq[512] = {};
        const std::wstring sec = L"Component" + std::to_wstring(i);
        if (!PortApi::ResolveSection(ns.c_str(), nullptr, sec.c_str(), fq, std::size(fq)) || !fq[0]) break;
        Component c;
        ReadComponent(fq, i, c);
        tail_to_fq[Lower(c.tail)] = fq;
        out.comps.push_back(std::move(c));
    }
    out.comps.erase(std::remove_if(out.comps.begin(), out.comps.end(),
                        [](const Component& c) { return c.match_index_count == 0 && c.draws.empty(); }),
        out.comps.end());
    const size_t numbered_count = out.comps.size();

    {
        struct RangedCtx {
            std::wstring              ns_lower;
            std::vector<std::wstring> fqs;
        } rc{ Lower(ns_fq), {} };
        PortApi::ForEachTextureOverride(
            [](uint32_t hash, const wchar_t* fq, bool ranged, const wchar_t*, void* user) {
                auto* c = (RangedCtx*)user;
                if (hash && ranged && fq && Lower(NsOfFq(fq)) == c->ns_lower) c->fqs.push_back(fq);
            },
            &rc);
        std::sort(rc.fqs.begin(), rc.fqs.end());
        int next = 0;
        for (const Component& c : out.comps) next = (std::max)(next, c.index + 1);
        for (const std::wstring& fq : rc.fqs) {
            const std::wstring tail = TailOfFq(fq);
            if (std::any_of(out.comps.begin(), out.comps.end(),
                    [&](const Component& c) { return _wcsicmp(c.tail.c_str(), tail.c_str()) == 0; }))
                continue;
            Component c;
            ReadComponent(fq, next, c);
            if (!c.ib_hash || !c.match_index_count) continue;
            if (out.dir.empty()) {
                wchar_t folder[512] = {};
                if (PortApi::SectionFolder(fq.c_str(), folder, std::size(folder))) out.dir = folder;
            }
            tail_to_fq[Lower(c.tail)] = fq;
            out.comps.push_back(std::move(c));
            ++next;
        }
    }

    // A WWMI geometry unit targets one native mesh (one IndexBuffer content_hash).
    // Mods may also place auxiliary ranged overrides (e.g. skip-only shadow/proxy/shell
    // meshes on a different IB hash) in the same namespace; mixing multiple IB
    // hashes inside one Mod::comps causes RT intent validation to reject the whole
    // mod. Keep the primary mesh's IB hash in Mod::comps and preserve verified
    // skip-only auxiliary meshes separately in Mod::aux_skip_meshes.
    {
        struct HashGroup {
            uint32_t hash = 0;
            uint64_t total_indices = 0;
            size_t   comp_count = 0;
            size_t   numbered_hits = 0;
        };
        std::vector<HashGroup> groups;
        for (size_t i = 0; i < out.comps.size(); ++i) {
            const uint32_t h = out.comps[i].ib_hash;
            if (!h) continue;
            auto it = std::find_if(groups.begin(), groups.end(), [&](const HashGroup& g) { return g.hash == h; });
            if (it == groups.end()) {
                groups.push_back({ h, 0, 0, 0 });
                it = groups.end() - 1;
            }
            it->total_indices += out.comps[i].match_index_count;
            ++it->comp_count;
            if (i < numbered_count) ++it->numbered_hits;
        }
        if (groups.size() > 1) {
            const uint64_t guid = out.object_guid > 0 ? (uint64_t)out.object_guid : 0ull;
            const auto     best = std::max_element(groups.begin(), groups.end(), [&](const HashGroup& a, const HashGroup& b) {
                const bool a_guid = guid != 0 && a.total_indices == guid;
                const bool b_guid = guid != 0 && b.total_indices == guid;
                if (a_guid != b_guid) return !a_guid && b_guid;
                if ((a.numbered_hits > 0) != (b.numbered_hits > 0)) return a.numbered_hits == 0;
                if (a.numbered_hits != b.numbered_hits) return a.numbered_hits < b.numbered_hits;
                if (a.total_indices != b.total_indices) return a.total_indices < b.total_indices;
                if (a.comp_count != b.comp_count) return a.comp_count < b.comp_count;
                return a.hash > b.hash;
            });
            const uint32_t primary_hash = best->hash;
            int            next_extra = 0;
            for (size_t i = 0; i < out.comps.size(); ++i) {
                if (i < numbered_count && out.comps[i].ib_hash == primary_hash)
                    next_extra = (std::max)(next_extra, out.comps[i].index + 1);
            }
            std::vector<Component> kept;
            kept.reserve(out.comps.size());
            for (size_t i = 0; i < out.comps.size(); ++i) {
                auto& c = out.comps[i];
                if (c.ib_hash == primary_hash) {
                    if (i >= numbered_count) c.index = next_extra++;
                    kept.push_back(std::move(c));
                } else {
                    const auto fq_it = tail_to_fq.find(Lower(c.tail));
                    const bool skip_only = fq_it != tail_to_fq.end() && PortApi::IsSkipOnlyTextureOverride(fq_it->second.c_str());
                    if (skip_only && c.ib_hash && c.match_index_count) {
                        auto am = std::find_if(out.aux_skip_meshes.begin(), out.aux_skip_meshes.end(),
                            [&](const AuxSkipMesh& m) { return m.ib_hash == c.ib_hash; });
                        if (am == out.aux_skip_meshes.end()) {
                            out.aux_skip_meshes.push_back(AuxSkipMesh{ c.ib_hash, {} });
                            am = out.aux_skip_meshes.end() - 1;
                        }
                        logger::write("WwmiMod: ns='%ls' kept auxiliary skip-only override '%ls' (hash=%08x != primary %08x)",
                            ns.c_str(), c.tail.c_str(), c.ib_hash, primary_hash);
                        c.index = (int)am->comps.size();
                        am->comps.push_back(std::move(c));
                    } else {
                        logger::write("WwmiMod: ns='%ls' excluded auxiliary ranged override '%ls' (hash=%08x != primary %08x)",
                            ns.c_str(), c.tail.c_str(), c.ib_hash, primary_hash);
                    }
                }
            }
            out.comps.swap(kept);
        }
    }

    {
        std::vector<RemapInfo> remaps;
        wchar_t                fq[512] = {};
        if (PortApi::ResolveSection(ns.c_str(), nullptr, L"RemapMergedSkeleton", fq, std::size(fq)) && fq[0]) {
            RemapCtx rc;
            rc.out = &remaps;
            PortApi::ForEachSectionKey(fq, &OnRemapKey, &rc);
        }
        for (const auto& ri : remaps)
            for (auto& c : out.comps)
                if (c.index == ri.comp_idx) {
                    c.remapped = true;
                    c.remap_id = ri.id;
                    c.remap_vg = ri.vg;
                }
    }

    // Resources, from their [Resource*] store, filtered to this namespace.
    {
        const std::wstring ns_lower = Lower(ns_fq);
        ResCtx             rc{ &ns_lower, &out.resources };
        PortApi::ForEachCustomResource(&OnResource, &rc);
        std::set<std::wstring> seen;
        std::vector<Resource>  uniq;
        uniq.reserve(out.resources.size());
        for (auto& r : out.resources)
            if (seen.insert(Lower(r.name)).second) uniq.push_back(std::move(r));
        out.resources.swap(uniq);
        for (const auto& r : out.resources)
            if (Lower(r.name) == L"resourceblendremapforwardbuffer") {
                out.forward_map_file = r.filename;
                break;
            }
    }

    PortApi::ForEachSectionInNs(
        ns_fq.c_str(),
        [](const wchar_t* fq, void* user) {
            bool* hit = (bool*)user;
            if (*hit) return;
            // The parser puts the section prefix before the namespace: Filter\<ns>\<suffix>.
            if (_wcsnicmp(fq, L"Filter", 6) == 0) {
                *hit = true;
                return;
            }
            PortApi::ForEachSectionLine(
                fq,
                [](const wchar_t* line, void* u) {
                    bool* h = (bool*)u;
                    if (!*h) *h = QueriesPsSlot(line);
                },
                user);
        },
        &out.queries_ps_slots);

    logger::write("WwmiMod: ns='%ls' -> %zu comp(s), %zu resource(s), guid=%d verts=%u dir='%ls'%s%s",
        ns.c_str(), out.comps.size(), out.resources.size(), out.object_guid,
        out.mesh_vertex_count, out.dir.c_str(),
        out.forward_map_file.empty() ? "" : " (has fwdmap)",
        out.queries_ps_slots ? " (asks what the game bound in ps-tN: every read slot published)" : "");
    return (int)out.comps.size();
}

} // namespace WwmiMod
