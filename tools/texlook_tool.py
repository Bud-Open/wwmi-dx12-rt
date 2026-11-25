#!/usr/bin/env python3
"""
texlook_tool.py -- Open-source modular TexLook (.texlook / tex_look.bin) and
TexLod (.lodcache) inspection, merging, per-role seed export, and TEX-UHD-AUDIT
slot mapping report tool for wwmi-dx12-rt.

100% generic, zero per-character / per-hash hardcoding, Python 3 stdlib only.
"""
from __future__ import annotations

import argparse
import json
import re
import struct
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, Iterable, List, Optional, Sequence, Set, Tuple

# ---- Binary format constants (exact match with source/main/tex_look.cpp) ----
MAGIC_TXLK = 0x4B4C5854  # "TXLK"
MAGIC_TXL2 = 0x324C5854  # "TXL2" (+ places)
MAGIC_TXL3 = 0x334C5854  # "TXL3" (+ places + mesh_places)
VALID_MAGICS = (MAGIC_TXLK, MAGIC_TXL2, MAGIC_TXL3)
ALGO_VERSION = 1
K_MIXED = 0xFFFFFFFF
K_PATCHES = 5
K_MAX_CELLS = 32

CODEC_NAMES = {
    1: "BC1",
    3: "BC3",
    4: "BC4",
    5: "BC5",
    7: "BC7",
}

VERDICT_NAMES = {
    0: "NoMatch",
    1: "Match",
    2: "Ref",
}

DXGI_FORMAT_NAMES = {
    0: "DXGI_FORMAT_UNKNOWN",
    27: "DXGI_FORMAT_R8G8B8A8_TYPELESS",
    28: "DXGI_FORMAT_R8G8B8A8_UNORM",
    29: "DXGI_FORMAT_R8G8B8A8_UNORM_SRGB",
    60: "DXGI_FORMAT_R8_TYPELESS",
    61: "DXGI_FORMAT_R8_UNORM",
    70: "DXGI_FORMAT_BC1_TYPELESS",
    71: "DXGI_FORMAT_BC1_UNORM",
    72: "DXGI_FORMAT_BC1_UNORM_SRGB",
    73: "DXGI_FORMAT_BC2_TYPELESS",
    74: "DXGI_FORMAT_BC2_UNORM",
    75: "DXGI_FORMAT_BC2_UNORM_SRGB",
    76: "DXGI_FORMAT_BC3_TYPELESS",
    77: "DXGI_FORMAT_BC3_UNORM",
    78: "DXGI_FORMAT_BC3_UNORM_SRGB",
    79: "DXGI_FORMAT_BC4_TYPELESS",
    80: "DXGI_FORMAT_BC4_UNORM",
    81: "DXGI_FORMAT_BC4_SNORM",
    82: "DXGI_FORMAT_BC5_TYPELESS",
    83: "DXGI_FORMAT_BC5_UNORM",
    84: "DXGI_FORMAT_BC5_SNORM",
    87: "DXGI_FORMAT_B8G8R8A8_UNORM",
    88: "DXGI_FORMAT_B8G8R8X8_UNORM",
    90: "DXGI_FORMAT_B8G8R8A8_TYPELESS",
    91: "DXGI_FORMAT_B8G8R8A8_UNORM_SRGB",
    92: "DXGI_FORMAT_B8G8R8X8_TYPELESS",
    93: "DXGI_FORMAT_B8G8R8X8_UNORM_SRGB",
    94: "DXGI_FORMAT_BC6H_TYPELESS",
    95: "DXGI_FORMAT_BC6H_UF16",
    96: "DXGI_FORMAT_BC6H_SF16",
    97: "DXGI_FORMAT_BC7_TYPELESS",
    98: "DXGI_FORMAT_BC7_UNORM",
    99: "DXGI_FORMAT_BC7_UNORM_SRGB",
}

FNV_PRIME_64 = 0x100000001B3
MASK64 = (1 << 64) - 1


def dxgi_name(fmt: int) -> str:
    return DXGI_FORMAT_NAMES.get(fmt, f"DXGI_FORMAT_{fmt}")


def fnv_place_of(ns: str, comp: int, ps_hash: int) -> int:
    """Exact port of TexLookPlaceOf()."""
    h = 0xCBF29CE484222325
    for ch in ns.lower():
        h = ((h ^ ord(ch)) * FNV_PRIME_64) & MASK64
    h = ((h ^ (comp & 0xFFFFFFFF)) * FNV_PRIME_64) & MASK64
    return ((h ^ (ps_hash & MASK64)) * FNV_PRIME_64) & MASK64


def fnv_mesh_pass_of(ib_hash: int, first_index: int, index_count: int, ps_hash: int) -> int:
    """Exact port of TexLookMeshPassOf()."""
    h = 0x84222325CBF29CE4
    for v in (ib_hash & 0xFFFFFFFF, first_index & 0xFFFFFFFF, index_count & 0xFFFFFFFF):
        h = ((h ^ v) * FNV_PRIME_64) & MASK64
    return ((h ^ (ps_hash & MASK64)) * FNV_PRIME_64) & MASK64


def fnv_slot_of(pass_hash: int, slot: int) -> int:
    """Exact port of TexLookSlotOf()."""
    return (((pass_hash & MASK64) ^ (slot & 0xFFFFFFFF)) * FNV_PRIME_64) & MASK64


# ---- Data structures ----
@dataclass
class LookRef:
    target: int
    codec: int
    nch: int
    w: int
    h: int
    pw: int
    ph: int
    px: bytes

    @property
    def cells(self) -> int:
        return self.pw * self.ph

    @property
    def codec_name(self) -> str:
        return CODEC_NAMES.get(self.codec, f"C{self.codec}")


@dataclass
class Verdict:
    tex_hash: int
    gen: int
    kind: int


@dataclass
class TexLookData:
    magic: int = MAGIC_TXL3
    algo: int = ALGO_VERSION
    gen: int = 1
    refs: Dict[int, LookRef] = field(default_factory=dict)
    verdicts: Dict[int, Verdict] = field(default_factory=dict)
    places: Dict[int, int] = field(default_factory=dict)
    mesh_places: Dict[int, int] = field(default_factory=dict)
    truncated: bool = False


@dataclass
class LodLink:
    high: int
    mip: int = 0
    exact: bool = False
    chain: bool = False
    place: bool = False
    ssim: float = 0.0


@dataclass
class LodCacheData:
    # low_hash -> list[LodLink]
    records: Dict[int, List[LodLink]] = field(default_factory=dict)
    degenerate: Set[int] = field(default_factory=set)

    def by_target(self) -> Dict[int, List[Tuple[int, LodLink]]]:
        out: Dict[int, List[Tuple[int, LodLink]]] = defaultdict(list)
        for low, links in self.records.items():
            for link in links:
                out[link.high].append((low, link))
        for items in out.values():
            items.sort(key=lambda x: x[0])
        return out


# ---- Binary .texlook / tex_look.bin parser & serializer ----
def read_texlook(path: Path) -> TexLookData:
    data = path.read_bytes()
    if len(data) < 20:
        raise ValueError(f"{path}: file too small ({len(data)} bytes)")
    magic, algo, gen, nrefs, nverdicts = struct.unpack_from("<5I", data, 0)
    if magic not in VALID_MAGICS:
        raise ValueError(f"{path}: unsupported magic {magic:#010x}")
    if algo != ALGO_VERSION:
        raise ValueError(f"{path}: unsupported algorithm version {algo} (expected {ALGO_VERSION})")

    res = TexLookData(magic=magic, algo=algo, gen=gen)
    off = 20

    for _ in range(nrefs):
        if off + 22 > len(data):
            res.truncated = True
            return res
        target, codec, nch, w, h, pw, ph = struct.unpack_from("<IBB4I", data, off)
        off += 22
        if not (1 <= nch <= 4 and 1 <= pw <= K_MAX_CELLS and 1 <= ph <= K_MAX_CELLS):
            res.truncated = True
            return res
        px_len = K_PATCHES * pw * ph * nch
        if off + px_len > len(data):
            res.truncated = True
            return res
        px = data[off : off + px_len]
        off += px_len
        res.refs[target] = LookRef(target=target, codec=codec, nch=nch, w=w, h=h, pw=pw, ph=ph, px=px)

    for _ in range(nverdicts):
        if off + 9 > len(data):
            res.truncated = True
            return res
        hval, vgen, kind = struct.unpack_from("<IIB", data, off)
        off += 9
        if kind > 2:
            res.truncated = True
            return res
        res.verdicts[hval] = Verdict(tex_hash=hval, gen=vgen, kind=kind)

    if magic in (MAGIC_TXL2, MAGIC_TXL3):
        if off + 4 > len(data):
            res.truncated = True
            return res
        (nplaces,) = struct.unpack_from("<I", data, off)
        off += 4
        for _ in range(nplaces):
            if off + 12 > len(data):
                res.truncated = True
                return res
            place, target = struct.unpack_from("<QI", data, off)
            off += 12
            if target:
                res.places[place] = target

    if magic == MAGIC_TXL3:
        if off + 4 > len(data):
            res.truncated = True
            return res
        (nmesh,) = struct.unpack_from("<I", data, off)
        off += 4
        for _ in range(nmesh):
            if off + 12 > len(data):
                res.truncated = True
                return res
            place, target = struct.unpack_from("<QI", data, off)
            off += 12
            if target:
                res.mesh_places[place] = target

    return res


def write_texlook(path: Path, data: TexLookData) -> int:
    chunks: List[bytes] = []
    chunks.append(
        struct.pack(
            "<5I",
            MAGIC_TXL3,
            ALGO_VERSION,
            data.gen & 0xFFFFFFFF,
            len(data.refs),
            len(data.verdicts),
        )
    )
    for target in sorted(data.refs.keys()):
        r = data.refs[target]
        chunks.append(struct.pack("<IBB4I", target, r.codec, r.nch, r.w, r.h, r.pw, r.ph))
        chunks.append(r.px)
    for hval in sorted(data.verdicts.keys()):
        v = data.verdicts[hval]
        chunks.append(struct.pack("<IIB", hval, v.gen, v.kind))
    chunks.append(struct.pack("<I", len(data.places)))
    for pl in sorted(data.places.keys()):
        chunks.append(struct.pack("<QI", pl, data.places[pl]))
    chunks.append(struct.pack("<I", len(data.mesh_places)))
    for pl in sorted(data.mesh_places.keys()):
        chunks.append(struct.pack("<QI", pl, data.mesh_places[pl]))

    payload = b"".join(chunks)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_bytes(payload)
    tmp.replace(path)
    return len(payload)


def merge_place_map(dst: Dict[int, int], src: Dict[int, int]) -> Tuple[int, int]:
    """Merge places using exact tex_look.cpp keep_place semantics. Returns (added, became_mixed)."""
    added = 0
    became_mixed = 0
    for pl, tgt in src.items():
        if not tgt:
            continue
        cur = dst.get(pl)
        if cur is None:
            dst[pl] = tgt
            added += 1
        elif cur != tgt and cur != K_MIXED:
            dst[pl] = K_MIXED
            became_mixed += 1
    return added, became_mixed


def merge_texlook(
    inputs: Sequence[TexLookData],
    *,
    drop_verdicts: bool = False,
    drop_mixed: bool = False,
) -> Tuple[TexLookData, Dict[str, int]]:
    out = TexLookData(magic=MAGIC_TXL3, algo=ALGO_VERSION, gen=1)
    stats = {
        "refs_added": 0,
        "refs_upgraded": 0,
        "refs_skipped_weaker": 0,
        "places_added": 0,
        "places_mixed": 0,
        "mesh_places_added": 0,
        "mesh_places_mixed": 0,
    }
    for item in inputs:
        out.gen = max(out.gen, item.gen)
        for target, ref in item.refs.items():
            cur = out.refs.get(target)
            if cur is None:
                out.refs[target] = ref
                stats["refs_added"] += 1
            elif ref.cells > cur.cells:
                out.refs[target] = ref
                out.gen += 1
                stats["refs_upgraded"] += 1
            else:
                stats["refs_skipped_weaker"] += 1
        if not drop_verdicts:
            for hval, v in item.verdicts.items():
                cur = out.verdicts.get(hval)
                if cur is None or v.gen > cur.gen or (v.gen == cur.gen and v.kind > cur.kind):
                    out.verdicts[hval] = v
        pa, pm = merge_place_map(out.places, item.places)
        ma, mm = merge_place_map(out.mesh_places, item.mesh_places)
        stats["places_added"] += pa
        stats["places_mixed"] += pm
        stats["mesh_places_added"] += ma
        stats["mesh_places_mixed"] += mm

    if drop_mixed:
        out.places = {k: v for k, v in out.places.items() if v != K_MIXED}
        out.mesh_places = {k: v for k, v in out.mesh_places.items() if v != K_MIXED}
    return out, stats


# ---- .lodcache parser & serializer ----
LODCACHE_HEADER = (
    "; wwmi-dx12 LOD texture matches. One line per target:  <target> = <low> <flags> | <low> <flags> ...\n"
    "; A low is a lower-resolution texture the game streams in; it is swapped as its target's mod texture.\n"
    ";   mip=K         the low is the target's mip K (0: the level is not known)\n"
    ";   exact=1       proven from the game's own data: the target's own mip, or a stream copy\n"
    ";   exact=0       only looks alike (tex_look, its reference in tex_look.bin): with lod_resemblance=1 swapped like\n"
    ";                 an exact low; under several targets, as the one ahead by 0.03 ssim, else not at all\n"
    ";   chain=1       learned through a chain of stream copies; a look-alike learned so has the ssim of the one it came through\n"
    ";   ssim=S        a look-alike's lowest patch similarity to the target's reference (1: identical)\n"
    ";   place=1       a look-alike by where it is drawn: HD drew the target at that slot of a mod's component\n"
    "; degenerate = <low> <low> ...  identical to the mips of more than one target: never swapped, never learned again\n"
)


def _merge_lod_link(records: Dict[int, List[LodLink]], low: int, link: LodLink) -> None:
    if not low or not link.high or low == link.high:
        return
    links = records.setdefault(low, [])
    for idx, cur in enumerate(links):
        if cur.high == link.high:
            links[idx] = link
            return
    links.append(link)


def normalize_lodcache(data: LodCacheData) -> None:
    for low in list(data.records.keys()):
        links = [k for k in data.records[low] if k.exact or k.ssim > 0.0]
        if any(k.exact for k in links):
            links = [k for k in links if k.exact]
        if links and links[0].exact and len(links) > 1:
            data.degenerate.add(low)
        if not links or low in data.degenerate:
            data.records.pop(low, None)
        else:
            data.records[low] = links


def read_lodcache(path: Path) -> LodCacheData:
    res = LodCacheData()
    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.strip()
        if not line or line.startswith(";"):
            continue
        if line.startswith("degenerate"):
            if "=" not in line:
                continue
            rhs = line.split("=", 1)[1]
            for tok in rhs.split():
                try:
                    val = int(tok, 16)
                    if val:
                        res.degenerate.add(val)
                except ValueError:
                    pass
            continue
        if "exact=" not in line or "=" not in line:
            continue
        lhs, rhs = line.split("=", 1)
        try:
            target = int(lhs.strip(), 16)
        except ValueError:
            continue
        if not target:
            continue
        for entry in rhs.split("|"):
            toks = entry.strip().split()
            if not toks:
                continue
            try:
                low = int(toks[0], 16)
            except ValueError:
                continue
            link = LodLink(high=target)
            flagged_degen = False
            for tok in toks[1:]:
                if "=" not in tok:
                    continue
                k, v = tok.split("=", 1)
                if k == "mip":
                    link.mip = int(v)
                elif k == "exact":
                    link.exact = int(v) != 0
                elif k == "chain":
                    link.chain = int(v) != 0
                elif k == "ssim":
                    link.ssim = float(v)
                elif k == "place":
                    link.place = int(v) != 0
                elif k == "degenerate":
                    flagged_degen = int(v) != 0
            if flagged_degen and link.exact:
                res.degenerate.add(low)
            else:
                _merge_lod_link(res.records, low, link)
    normalize_lodcache(res)
    return res


def write_lodcache(path: Path, data: LodCacheData, *, exact_only: bool = False) -> int:
    by_tgt: Dict[int, List[Tuple[int, LodLink]]] = defaultdict(list)
    for low, links in data.records.items():
        if low in data.degenerate:
            continue
        for link in links:
            if exact_only and not link.exact:
                continue
            by_tgt[link.high].append((low, link))

    lines = [LODCACHE_HEADER]
    for tgt in sorted(by_tgt.keys()):
        items = sorted(by_tgt[tgt], key=lambda x: x[0])
        parts = []
        for low, k in items:
            seg = f"{low:08x} mip={k.mip} exact={int(k.exact)} chain={int(k.chain)}"
            if not k.exact:
                seg += f" ssim={k.ssim:.3f}"
            if k.place:
                seg += " place=1"
            parts.append(seg)
        lines.append(f"{tgt:08x} = " + " | ".join(parts) + "\n")
    if data.degenerate:
        degen_s = " ".join(f"{h:08x}" for h in sorted(data.degenerate))
        lines.append(f"degenerate = {degen_s}\n")

    content = "".join(lines)
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + ".tmp")
    tmp.write_text(content, encoding="utf-8")
    tmp.replace(path)
    return len(content.encode("utf-8"))


def merge_lodcache(inputs: Sequence[LodCacheData], *, exact_only: bool = False) -> Tuple[LodCacheData, Dict[str, int]]:
    out = LodCacheData()
    stats = {
        "exact_added": 0,
        "lookalike_added": 0,
        "lookalike_promoted": 0,
        "mip_filled": 0,
        "conflict_degenerate": 0,
        "degenerate_total": 0,
    }
    for src in inputs:
        out.degenerate.update(src.degenerate)
    for low in list(out.degenerate):
        out.records.pop(low, None)

    for src in inputs:
        for low, links in src.records.items():
            if low in out.degenerate:
                continue
            for link in links:
                if exact_only and not link.exact:
                    continue
                if low in out.degenerate:
                    break
                cur = out.records.get(low)
                if cur is None:
                    out.records[low] = [LodLink(**vars(link))]
                    if link.exact:
                        stats["exact_added"] += 1
                    else:
                        stats["lookalike_added"] += 1
                    continue
                cur_exact = cur[0].exact
                if link.exact:
                    if not cur_exact:
                        out.records[low] = [LodLink(**vars(link))]
                        stats["lookalike_promoted"] += 1
                    else:
                        e = cur[0]
                        if e.high == link.high:
                            if not e.mip and link.mip:
                                e.mip = link.mip
                                stats["mip_filled"] += 1
                        else:
                            out.records.pop(low, None)
                            out.degenerate.add(low)
                            stats["conflict_degenerate"] += 1
                else:
                    if cur_exact:
                        continue
                    existing = next((c for c in cur if c.high == link.high), None)
                    if existing is None:
                        cur.append(LodLink(**vars(link)))
                        stats["lookalike_added"] += 1
                    elif link.ssim > existing.ssim:
                        existing.ssim = link.ssim
                        existing.place = existing.place or link.place

    normalize_lodcache(out)
    stats["degenerate_total"] = len(out.degenerate)
    return out, stats


# ---- Generic WWMI mod.ini parser & TEX-UHD-AUDIT log parser ----
@dataclass
class ModComponentMeta:
    index: int
    section: str
    ib_hash: int = 0
    first_index: int = 0
    index_count: int = 0


@dataclass
class ModIniMeta:
    path: Path
    object_guid: int = 0
    components: Dict[int, ModComponentMeta] = field(default_factory=dict)
    texture_override_hashes: Set[int] = field(default_factory=set)
    resource_filename_hashes: Set[int] = field(default_factory=set)


_RE_SECTION = re.compile(r"^\[(.+)\]$")
_RE_COMP_SEC = re.compile(r"^TextureOverrideComponent(\d+)$", re.I)
_RE_TEX_OVR = re.compile(r"^TextureOverrideTexture", re.I)
_RE_RES_TEX = re.compile(r"^ResourceTexture", re.I)
_RE_FILE_HASH = re.compile(r"\bt=([0-9a-fA-F]{8})\b")
_RE_GUID = re.compile(r"^(?:global\s+)?\$object_guid\s*=\s*(\d+)", re.I)


def parse_mod_ini_meta(path: Path) -> ModIniMeta:
    meta = ModIniMeta(path=path)
    cur_sec: Optional[str] = None
    cur_comp: Optional[ModComponentMeta] = None
    in_tex_ovr = False
    in_res_tex = False

    for raw in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = raw.split(";", 1)[0].strip()
        if not line:
            continue
        m_sec = _RE_SECTION.match(line)
        if m_sec:
            cur_sec = m_sec.group(1).strip()
            cur_comp = None
            in_tex_ovr = bool(_RE_TEX_OVR.match(cur_sec))
            in_res_tex = bool(_RE_RES_TEX.match(cur_sec))
            mc = _RE_COMP_SEC.match(cur_sec)
            if mc:
                idx = int(mc.group(1))
                cur_comp = meta.components.setdefault(idx, ModComponentMeta(index=idx, section=cur_sec))
            continue

        mg = _RE_GUID.match(line)
        if mg and not meta.object_guid:
            meta.object_guid = int(mg.group(1))
            continue

        if "=" not in line:
            continue
        k, v = [p.strip() for p in line.split("=", 1)]
        kl = k.lower()
        if cur_comp is not None:
            if kl == "hash":
                try:
                    cur_comp.ib_hash = int(v, 16)
                except ValueError:
                    pass
            elif kl == "match_first_index":
                try:
                    cur_comp.first_index = int(v)
                except ValueError:
                    pass
            elif kl == "match_index_count":
                try:
                    cur_comp.index_count = int(v)
                except ValueError:
                    pass
        elif in_tex_ovr and kl == "hash":
            try:
                hval = int(v, 16)
                if hval:
                    meta.texture_override_hashes.add(hval)
            except ValueError:
                pass
        elif in_res_tex and kl == "filename":
            mf = _RE_FILE_HASH.search(v)
            if mf:
                meta.resource_filename_hashes.add(int(mf.group(1), 16))
    return meta


@dataclass
class AuditRow:
    gen: int
    mod: int
    guid: int
    comp: int
    rtv: int
    slot: int
    width: int
    height: int
    mips: int
    fmt: int
    vfmt: int
    tex_hash: int
    rule: int
    lod: int
    sub1: int
    sub2: int
    via: int
    swapped: int
    ovr: int


_RE_AUDIT = re.compile(
    r"TEX-UHD-AUDIT\s+"
    r"gen=(\d+)\s+mod=(\d+)\s+guid=(\d+)\s+comp=(\d+)\s+rtv=(\d+)\s+t(\d+)\s+"
    r"res=(\d+)x(\d+)\s+m(\d+)\s+fmt=(\d+)\s+vfmt=(\d+)\s+"
    r"hash=([0-9a-fA-F]+)\s+rule=([0-9a-fA-F]+)\s+lod=([0-9a-fA-F]+)\s+"
    r"sub1=([0-9a-fA-F]+)\s+sub2=([0-9a-fA-F]+)\s+via=(\d+)\s+swapped=(\d+)\s+ovr=(\d+)"
)
_RE_PS_HASH = re.compile(r"\bps=([0-9a-fA-F]{16})\b")


def parse_audit_log(path: Path) -> Tuple[List[AuditRow], Set[int]]:
    rows: List[AuditRow] = []
    ps_hashes: Set[int] = set()
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        if "TEX-UHD-AUDIT" in line:
            m = _RE_AUDIT.search(line)
            if m:
                rows.append(
                    AuditRow(
                        gen=int(m.group(1)),
                        mod=int(m.group(2)),
                        guid=int(m.group(3)),
                        comp=int(m.group(4)),
                        rtv=int(m.group(5)),
                        slot=int(m.group(6)),
                        width=int(m.group(7)),
                        height=int(m.group(8)),
                        mips=int(m.group(9)),
                        fmt=int(m.group(10)),
                        vfmt=int(m.group(11)),
                        tex_hash=int(m.group(12), 16),
                        rule=int(m.group(13), 16),
                        lod=int(m.group(14), 16),
                        sub1=int(m.group(15), 16),
                        sub2=int(m.group(16), 16),
                        via=int(m.group(17)),
                        swapped=int(m.group(18)),
                        ovr=int(m.group(19)),
                    )
                )
        else:
            for pm in _RE_PS_HASH.finditer(line):
                val = int(pm.group(1), 16)
                if val:
                    ps_hashes.add(val)
    return rows, ps_hashes


def detect_file_kind(path: Path) -> str:
    raw = path.read_bytes()[:4]
    if len(raw) == 4:
        (magic,) = struct.unpack("<I", raw)
        if magic in VALID_MAGICS:
            return "texlook"
    return "lodcache"


# ---- Report builder (TEX-UHD-AUDIT + tex_look.bin + lodcache + optional mod.ini) ----
def build_audit_report(
    audit_rows: Sequence[AuditRow],
    *,
    texlook: Optional[TexLookData] = None,
    lodcache: Optional[LodCacheData] = None,
    mod_meta: Optional[ModIniMeta] = None,
    guid_filter: Optional[int] = None,
) -> Dict[str, object]:
    if guid_filter is None and mod_meta is not None and mod_meta.object_guid:
        guid_filter = mod_meta.object_guid
    filtered = [r for r in audit_rows if guid_filter is None or r.guid == guid_filter]

    # Detect generic engine/shared textures: identical hash seen across >= 4 distinct components
    # at small resolution <= 256x256 and without a character-specific lod target.
    hash_comps: Dict[int, Set[int]] = defaultdict(set)
    hash_max_dim: Dict[int, int] = defaultdict(int)
    hash_has_lod: Dict[int, bool] = defaultdict(bool)
    for r in filtered:
        if r.tex_hash:
            hash_comps[r.tex_hash].add(r.comp)
            hash_max_dim[r.tex_hash] = max(hash_max_dim[r.tex_hash], r.width, r.height)
            if r.lod or r.rule:
                hash_has_lod[r.tex_hash] = True
    shared_engine_hashes = {
        h
        for h, comps in hash_comps.items()
        if len(comps) >= 4 and hash_max_dim[h] <= 256 and not hash_has_lod[h]
    }

    mesh_place_counts: Counter[int] = Counter()
    if texlook is not None:
        for tgt in texlook.mesh_places.values():
            if tgt != K_MIXED:
                mesh_place_counts[tgt] += 1

    lod_index: Dict[int, List[LodLink]] = lodcache.records if lodcache is not None else {}
    mod_hashes = (mod_meta.texture_override_hashes | mod_meta.resource_filename_hashes) if mod_meta else set()

    # Group by (guid, comp, slot, vfmt) so multi-pass slots with distinct formats stay visible!
    grouped: Dict[Tuple[int, int, int, int], List[AuditRow]] = defaultdict(list)
    for r in filtered:
        grouped[(r.guid, r.comp, r.slot, r.vfmt)].append(r)

    entries = []
    status_counts: Counter[str] = Counter()
    for (guid, comp, slot, vfmt), rows in sorted(grouped.items()):
        max_w = max(r.width for r in rows)
        max_h = max(r.height for r in rows)
        max_mips = max(r.mips for r in rows)
        fmt = rows[0].fmt
        live_hashes = sorted({r.tex_hash for r in rows if r.tex_hash})
        rules = sorted({r.rule for r in rows if r.rule})
        lods = sorted({r.lod for r in rows if r.lod})
        any_ovr = any(r.ovr or r.swapped for r in rows)

        # Resolve SSIM / exact provenance from lodcache when available
        lod_details = []
        for lh in live_hashes:
            for link in lod_index.get(lh, []):
                lod_details.append(
                    {
                        "live_hash": f"{lh:08x}",
                        "target_hash": f"{link.high:08x}",
                        "exact": link.exact,
                        "mip": link.mip,
                        "ssim": round(link.ssim, 4) if not link.exact else 1.0,
                        "place": link.place,
                    }
                )
                if link.high not in lods:
                    lods.append(link.high)
        lods = sorted(set(lods))

        seed_refs = []
        if texlook is not None:
            for th in lods or live_hashes:
                ref = texlook.refs.get(th)
                if ref is not None:
                    seed_refs.append(
                        {
                            "target_hash": f"{th:08x}",
                            "codec": ref.codec_name,
                            "dims": f"{ref.w}x{ref.h}",
                            "grid": f"{ref.pw}x{ref.ph}",
                            "mesh_places_hits": mesh_place_counts.get(th, 0),
                        }
                    )

        is_placeholder = max_w <= 64 or max_h <= 64 or max_w != max_h
        is_shared_engine = bool(live_hashes) and all(h in shared_engine_hashes for h in live_hashes) and not any_ovr and not rules and not lods

        if is_placeholder or is_shared_engine:
            status = "ENGINE_OR_PLACEHOLDER"
        elif rules:
            status = "MATCHED_EXACT"
        elif any_ovr:
            status = "OVERRIDDEN_BY_MOD"
        elif lods:
            if mod_hashes and any(t in mod_hashes for t in lods):
                status = "MATCHED_VIA_LOD"
            else:
                status = "DRIFTED_SEED_RECOVERED"
        else:
            status = "UNSEEDED_UNMATCHED"

        status_counts[status] += 1
        entries.append(
            {
                "guid": guid,
                "comp": comp,
                "slot": f"t{slot}",
                "slot_index": slot,
                "max_res": f"{max_w}x{max_h}",
                "max_mips": max_mips,
                "fmt": fmt,
                "vfmt": vfmt,
                "vfmt_name": dxgi_name(vfmt),
                "live_hashes": [f"{h:08x}" for h in live_hashes],
                "matched_rules": [f"{h:08x}" for h in rules],
                "lod_targets": [f"{h:08x}" for h in lods],
                "lod_details": lod_details,
                "seed_refs": seed_refs,
                "overridden": any_ovr,
                "status": status,
            }
        )

    return {
        "guid_filter": guid_filter,
        "audit_rows_total": len(audit_rows),
        "audit_rows_filtered": len(filtered),
        "shared_engine_hashes": [f"{h:08x}" for h in sorted(shared_engine_hashes)],
        "status_summary": dict(sorted(status_counts.items())),
        "slots": entries,
    }


def format_report_markdown(report: Dict[str, object]) -> str:
    lines = [
        "# TEX-UHD-AUDIT & TexLook Component Slot Report",
        "",
        f"- GUID Filter: `{report['guid_filter']}`",
        f"- Audit Rows: `{report['audit_rows_filtered']}` / `{report['audit_rows_total']}`",
        f"- Shared Engine/Noise Hashes Filtered: `{', '.join(report['shared_engine_hashes']) or 'none'}`",
        f"- Status Summary: `{json.dumps(report['status_summary'], ensure_ascii=False)}`",
        "",
        "| Comp | Slot | Max Res | View Format | Live UHD Hashes | Rule | TexLook/Lod Target | Seed Ref | Status |",
        "|---:|---:|---|---|---|---|---|---|---|",
    ]
    for s in report["slots"]:
        if s["status"] == "ENGINE_OR_PLACEHOLDER":
            continue
        live_s = ", ".join(s["live_hashes"][:3]) or "-"
        rule_s = ", ".join(s["matched_rules"]) or "-"
        lod_s = ", ".join(s["lod_targets"]) or "-"
        ref_s = ", ".join(f"{r['target_hash']}({r['codec']} {r['dims']})" for r in s["seed_refs"]) or "-"
        lines.append(
            f"| {s['comp']} | `{s['slot']}` | {s['max_res']} m{s['max_mips']} | `{s['vfmt_name']}` ({s['vfmt']}) "
            f"| `{live_s}` | `{rule_s}` | `{lod_s}` | {ref_s} | `{s['status']}` |"
        )
    lines.append("")
    return "\n".join(lines)


# ---- CLI command implementations ----
def cmd_inspect(args: argparse.Namespace) -> int:
    path = Path(args.file)
    kind = args.type if args.type != "auto" else detect_file_kind(path)
    if kind == "texlook":
        tl = read_texlook(path)
        by_codec = Counter(r.codec_name for r in tl.refs.values())
        by_dims = Counter(f"{r.w}x{r.h}" for r in tl.refs.values())
        by_verdict = Counter(VERDICT_NAMES.get(v.kind, str(v.kind)) for v in tl.verdicts.values())
        places_mixed = sum(1 for v in tl.places.values() if v == K_MIXED)
        mesh_mixed = sum(1 for v in tl.mesh_places.values() if v == K_MIXED)
        unique_mesh_targets = len({v for v in tl.mesh_places.values() if v != K_MIXED})
        summary = {
            "file": str(path),
            "kind": "texlook",
            "bytes": path.stat().st_size,
            "magic": f"{tl.magic:#010x}",
            "algo": tl.algo,
            "gen": tl.gen,
            "truncated": tl.truncated,
            "refs": len(tl.refs),
            "refs_by_codec": dict(sorted(by_codec.items())),
            "refs_by_dims": dict(sorted(by_dims.items())),
            "verdicts": len(tl.verdicts),
            "verdicts_by_kind": dict(sorted(by_verdict.items())),
            "places": len(tl.places),
            "places_mixed": places_mixed,
            "mesh_places": len(tl.mesh_places),
            "mesh_places_mixed": mesh_mixed,
            "mesh_places_unique_targets": unique_mesh_targets,
        }
    else:
        lc = read_lodcache(path)
        exact_lows = sum(1 for links in lc.records.values() if links and links[0].exact)
        look_lows = len(lc.records) - exact_lows
        place_links = sum(1 for links in lc.records.values() for k in links if k.place)
        by_tgt = lc.by_target()
        summary = {
            "file": str(path),
            "kind": "lodcache",
            "bytes": path.stat().st_size,
            "targets": len(by_tgt),
            "lows_total": len(lc.records),
            "exact_lows": exact_lows,
            "lookalike_lows": look_lows,
            "place_links": place_links,
            "degenerate": len(lc.degenerate),
        }

    if args.json:
        print(json.dumps(summary, indent=2, ensure_ascii=False))
    else:
        for k, v in summary.items():
            print(f"{k}: {v}")
    return 0


def cmd_merge(args: argparse.Namespace) -> int:
    in_paths = [Path(p) for p in args.inputs]
    kind = args.type if args.type != "auto" else detect_file_kind(in_paths[0])
    out_path = Path(args.out)

    if kind == "texlook":
        items = [read_texlook(p) for p in in_paths]
        merged, stats = merge_texlook(items, drop_verdicts=args.drop_verdicts, drop_mixed=args.drop_mixed)
        written = write_texlook(out_path, merged)
        print(
            f"Merged {len(items)} texlook file(s) -> {out_path} ({written:,} bytes): "
            f"refs={len(merged.refs)} verdicts={len(merged.verdicts)} "
            f"places={len(merged.places)} mesh_places={len(merged.mesh_places)} | {stats}"
        )
    else:
        items = [read_lodcache(p) for p in in_paths]
        merged, stats = merge_lodcache(items, exact_only=args.exact_only)
        written = write_lodcache(out_path, merged, exact_only=args.exact_only)
        print(
            f"Merged {len(items)} lodcache file(s) -> {out_path} ({written:,} bytes): "
            f"lows={len(merged.records)} degenerate={len(merged.degenerate)} | {stats}"
        )
    return 0


def cmd_export_role(args: argparse.Namespace) -> int:
    tl = read_texlook(Path(args.texlook))
    selected_targets: Set[int] = set()
    selected_mesh_places: Dict[int, int] = {}
    selected_places: Dict[int, int] = {}

    mod_meta: Optional[ModIniMeta] = None
    if args.mod_ini:
        mod_meta = parse_mod_ini_meta(Path(args.mod_ini))
        selected_targets.update(mod_meta.texture_override_hashes)
        selected_targets.update(mod_meta.resource_filename_hashes)

    audit_ps_hashes: Set[int] = set()
    if args.audit_log:
        rows, audit_ps_hashes = parse_audit_log(Path(args.audit_log))
        guid = args.guid if args.guid is not None else (mod_meta.object_guid if mod_meta else None)
        for r in rows:
            if guid is not None and r.guid != guid:
                continue
            if r.rule:
                selected_targets.add(r.rule)
            if r.lod:
                selected_targets.add(r.lod)

    if args.targets:
        for tok in args.targets.split(","):
            tok = tok.strip()
            if tok:
                selected_targets.add(int(tok, 16))

    extra_ps: Set[int] = set(audit_ps_hashes)
    if args.ps_hashes:
        for tok in args.ps_hashes.split(","):
            tok = tok.strip()
            if tok:
                extra_ps.add(int(tok, 16))

    # Reverse-resolve mesh_places by component IB/first/count + PS hashes when provided
    if mod_meta is not None and extra_ps:
        for comp in mod_meta.components.values():
            if not comp.ib_hash or not comp.index_count:
                continue
            for ps in extra_ps:
                mpass = fnv_mesh_pass_of(comp.ib_hash, comp.first_index, comp.index_count, ps)
                for slot in range(16):
                    pl = fnv_slot_of(mpass, slot)
                    tgt = tl.mesh_places.get(pl)
                    if tgt and tgt != K_MIXED:
                        selected_targets.add(tgt)
                        selected_mesh_places[pl] = tgt

    # Also keep every place / mesh_place referencing any selected target
    for pl, tgt in tl.mesh_places.items():
        if tgt in selected_targets:
            selected_mesh_places[pl] = tgt
    for pl, tgt in tl.places.items():
        if tgt in selected_targets:
            selected_places[pl] = tgt

    out_tl = TexLookData(
        magic=MAGIC_TXL3,
        algo=ALGO_VERSION,
        gen=tl.gen,
        refs={t: tl.refs[t] for t in selected_targets if t in tl.refs},
        verdicts={} if args.drop_verdicts else {h: v for h, v in tl.verdicts.items() if h in selected_targets},
        places=selected_places,
        mesh_places=selected_mesh_places,
    )
    out_path = Path(args.out)
    written = write_texlook(out_path, out_tl)
    print(
        f"Exported role seed -> {out_path} ({written:,} bytes): "
        f"matched_targets={len(selected_targets)} refs={len(out_tl.refs)} "
        f"places={len(out_tl.places)} mesh_places={len(out_tl.mesh_places)}"
    )

    if args.lodcache and args.out_lodcache:
        lc = read_lodcache(Path(args.lodcache))
        out_lc = LodCacheData()
        for low, links in lc.records.items():
            kept = [k for k in links if k.high in selected_targets and (not args.exact_only or k.exact)]
            if kept:
                out_lc.records[low] = kept
        written_lc = write_lodcache(Path(args.out_lodcache), out_lc, exact_only=args.exact_only)
        print(
            f"Exported role lodcache -> {args.out_lodcache} ({written_lc:,} bytes): "
            f"lows={len(out_lc.records)}"
        )
    return 0


def cmd_report(args: argparse.Namespace) -> int:
    rows, _ = parse_audit_log(Path(args.audit_log))
    tl = read_texlook(Path(args.texlook)) if args.texlook else None
    lc = read_lodcache(Path(args.lodcache)) if args.lodcache else None
    mod_meta = parse_mod_ini_meta(Path(args.mod_ini)) if args.mod_ini else None

    report = build_audit_report(
        rows,
        texlook=tl,
        lodcache=lc,
        mod_meta=mod_meta,
        guid_filter=args.guid,
    )
    md = format_report_markdown(report)
    if args.json:
        Path(args.json).parent.mkdir(parents=True, exist_ok=True)
        Path(args.json).write_text(json.dumps(report, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    if args.md:
        Path(args.md).parent.mkdir(parents=True, exist_ok=True)
        Path(args.md).write_text(md, encoding="utf-8")
    print(md)
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="texlook_tool.py",
        description="Modular TexLook (.texlook) & TexLod (.lodcache) seed management and TEX-UHD-AUDIT report tool.",
    )
    sub = p.add_subparsers(dest="cmd", required=True)

    pi = sub.add_parser("inspect", help="Parse and summarize a .texlook / tex_look.bin or .lodcache file")
    pi.add_argument("file", help="Path to .texlook, tex_look.bin, or .lodcache")
    pi.add_argument("--type", choices=["auto", "texlook", "lodcache"], default="auto")
    pi.add_argument("--json", action="store_true", help="Emit JSON output")
    pi.set_defaults(func=cmd_inspect)

    pm = sub.add_parser("merge", help="Merge and deduplicate multiple .texlook or .lodcache files")
    pm.add_argument("--out", required=True, help="Output file path")
    pm.add_argument("--type", choices=["auto", "texlook", "lodcache"], default="auto")
    pm.add_argument("--drop-verdicts", action="store_true", help="Strip session verdicts when creating distributable .texlook seeds")
    pm.add_argument("--drop-mixed", action="store_true", help="Strip kMixed (0xffffffff) place conflicts")
    pm.add_argument("--exact-only", action="store_true", help="Keep only exact=1 proven lows when merging .lodcache")
    pm.add_argument("inputs", nargs="+", help="Input seed/cache files in overlay order")
    pm.set_defaults(func=cmd_merge)

    pe = sub.add_parser("export-role", help="Export a compact per-role .texlook (and optional .lodcache) seed slice")
    pe.add_argument("--texlook", required=True, help="Source .texlook or tex_look.bin")
    pe.add_argument("--out", required=True, help="Output per-role .texlook path")
    pe.add_argument("--mod-ini", help="WWMI mod.ini to extract component IB ranges and texture hashes")
    pe.add_argument("--audit-log", help="wwmi_dx12 log containing TEX-UHD-AUDIT lines")
    pe.add_argument("--guid", type=int, help="Explicit object_guid filter for --audit-log")
    pe.add_argument("--targets", help="Comma-separated hex target hashes to include")
    pe.add_argument("--ps-hashes", help="Comma-separated 64-bit hex PS hashes for mesh_places inversion")
    pe.add_argument("--lodcache", help="Optional source .lodcache to slice alongside .texlook")
    pe.add_argument("--out-lodcache", help="Optional output sliced .lodcache path")
    pe.add_argument("--drop-verdicts", action="store_true", default=True, help="Drop session verdicts (default: true)")
    pe.add_argument("--exact-only", action="store_true", help="Only export exact=1 links in --out-lodcache")
    pe.set_defaults(func=cmd_export_role)

    pr = sub.add_parser("report", help="Generate component slot mapping report from TEX-UHD-AUDIT + tex_look.bin")
    pr.add_argument("--audit-log", required=True, help="wwmi_dx12 log file containing TEX-UHD-AUDIT lines")
    pr.add_argument("--texlook", help="tex_look.bin or seed .texlook")
    pr.add_argument("--lodcache", help="lod_tex_cache.lodcache")
    pr.add_argument("--mod-ini", help="Optional WWMI mod.ini for hash-drift comparison")
    pr.add_argument("--guid", type=int, help="Optional object_guid filter")
    pr.add_argument("--json", help="Write structured JSON report to this path")
    pr.add_argument("--md", help="Write Markdown report to this path")
    pr.set_defaults(func=cmd_report)

    return p


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return int(args.func(args))


if __name__ == "__main__":
    raise SystemExit(main())
