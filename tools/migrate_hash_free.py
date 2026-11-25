#!/usr/bin/env python3
"""
migrate_hash_free.py -- Open-source, non-destructive Hash -> Hash-Free (ps-tN->Format)
migration tool for classic WWMI texture-hash mods.

100% generic, zero per-character / per-GUID / per-hash hardcoding.
Combines four universal evidence layers:
  A) Cross-patch invariant anchors (e.g., R8_UNORM / B8G8R8A8_UNORM exact rule hits)
  B) Component ownership (parsed from standard WWMI DDS filenames) + DDS header DXGI format
  C) Unambiguous 1-to-1 (Component, Slot, DXGI_FORMAT) structural matching
  D) Optional 5-patch TexLook visual signature comparison (via vendored MIT/Unlicense bcdec.h)
     with exact maximum-weight 1-to-1 bipartite assignment per component to resolve multi-file
     same-format slots (and author BC3->BC7 re-saves) without blind guessing.

Never mutates the original mod folder unless explicitly requested; emits:
  - migration_report.md   : human-readable slot-by-slot audit and confidence breakdown
  - slot_mapping.json     : editable mapping file (re-usable via --mapping-override)
  - hash_free_snippet.ini : standalone Hash-Free delta snippet
  - mod_hash_free.ini     : complete non-destructive Hash-Free mod.ini
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from collections import defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Set, Tuple

THIS_DIR = Path(__file__).resolve().parent
sys.path.insert(0, str(THIS_DIR))
import texlook_tool as tl  # noqa: E402


# ---- DDS header parsing (pure Python, zero external dependencies) ----
@dataclass
class DdsHeaderMeta:
    width: int
    height: int
    mips: int
    dxgi_format: int
    data_offset: int
    fourcc: str


def read_dds_header(path: Path) -> Optional[DdsHeaderMeta]:
    if not path.is_file():
        return None
    with path.open("rb") as f:
        hdr = f.read(148)
    if len(hdr) < 128 or hdr[:4] != b"DDS ":
        return None
    height, width = struct.unpack_from("<II", hdr, 12)
    mips = struct.unpack_from("<I", hdr, 28)[0] or 1
    pf_flags, fourcc_b, rgb_bits, rmask, gmask, bmask, amask = struct.unpack_from("<I4sIIIII", hdr, 80)
    fourcc = fourcc_b.decode("latin1")
    dxgi = 0
    data_off = 128

    if pf_flags & 0x4:  # DDPF_FOURCC
        if fourcc_b == b"DX10" and len(hdr) >= 148:
            (dxgi,) = struct.unpack_from("<I", hdr, 128)
            data_off = 148
        elif fourcc_b == b"DXT1":
            dxgi = 71
        elif fourcc_b == b"DXT3":
            dxgi = 74
        elif fourcc_b == b"DXT5":
            dxgi = 77
        elif fourcc_b in (b"ATI1", b"BC4U"):
            dxgi = 80
        elif fourcc_b in (b"ATI2", b"BC5U"):
            dxgi = 83
    elif rgb_bits == 32:
        if rmask == 0x00FF0000 and gmask == 0x0000FF00 and bmask == 0x000000FF:
            dxgi = 87 if amask else 88
        elif rmask == 0x000000FF and gmask == 0x0000FF00 and bmask == 0x00FF0000:
            dxgi = 28
    elif rgb_bits == 8:
        dxgi = 61

    if width == 0 or height == 0 or dxgi == 0:
        return None
    return DdsHeaderMeta(
        width=width,
        height=height,
        mips=mips,
        dxgi_format=dxgi,
        data_offset=data_off,
        fourcc=fourcc.strip("\x00"),
    )


def codec_of_dxgi(fmt: int) -> Tuple[int, int]:
    if 70 <= fmt <= 72:
        return (1, 3)
    if 76 <= fmt <= 78:
        return (3, 4)
    if fmt in (79, 80):
        return (4, 1)
    if fmt in (82, 83):
        return (5, 2)
    if 97 <= fmt <= 99:
        return (7, 4)
    return (0, 0)


# ---- Optional C bcdec 5-patch signature helper (exact match with tex_look.cpp) ----
_BCDEC_HELPER_C = r"""
#define BCDEC_IMPLEMENTATION
#include "bcdec.h"
#include <stdint.h>
#include <string.h>
#include <math.h>

static void decode_blk(uint8_t codec, const uint8_t* blk, uint8_t rgba[64]) {
    memset(rgba, 0, 64);
    if (codec == 1) bcdec_bc1(blk, rgba, 16);
    else if (codec == 3) bcdec_bc3(blk, rgba, 16);
    else if (codec == 7) bcdec_bc7(blk, rgba, 16);
    else if (codec == 4) {
        uint8_t r[16];
        bcdec_bc4(blk, r, 4);
        for (int i = 0; i < 16; ++i) rgba[i * 4] = r[i];
    } else if (codec == 5) {
        uint8_t rg[32];
        bcdec_bc5(blk, rg, 8);
        for (int i = 0; i < 16; ++i) {
            rgba[i * 4] = rg[i * 2];
            rgba[i * 4 + 1] = rg[i * 2 + 1];
        }
    }
}

int wwmi_dds_look_sig(uint8_t codec, uint8_t nch, uint32_t w, uint32_t h,
                      const uint8_t* bc, size_t bc_size,
                      uint32_t* out_pw, uint32_t* out_ph, uint8_t* out_px,
                      double* out_stats /* mean[4], std[4] */) {
    if (!codec || !nch || nch > 4 || w < 128 || h < 128) return 0;
    if ((w & (w - 1)) || (h & (h - 1))) return 0;
    uint32_t bb = (codec == 1 || codec == 4) ? 8 : 16;
    size_t row = (size_t)(w / 4) * bb;
    if (bc_size < row * (h / 4)) return 0;
    uint32_t ref_side = 512;
    uint32_t sw = w / 16, sh = h / 16;
    uint32_t fx = w / (w < ref_side ? w : ref_side);
    uint32_t fy = h / (h < ref_side ? h : ref_side);
    uint32_t pw = sw / fx, ph = sh / fy;
    if (pw == 0 || ph == 0 || pw > 32 || ph > 32) return 0;
    *out_pw = pw;
    *out_ph = ph;
    uint32_t iw = w / 8, ih = h / 8;
    uint32_t ax[5] = { (w - sw) / 2, iw, w - iw - sw, iw, w - iw - sw };
    uint32_t ay[5] = { (h - sh) / 2, ih, ih, h - ih - sh, h - ih - sh };
    uint32_t nbx = sw / 4, nby = sh / 4;
    size_t cells = (size_t)pw * ph * nch;
    uint32_t sum[32 * 32 * 4];
    double per = (double)fx * fy;
    double g_sum[4] = {0}, g_ss[4] = {0};
    double g_n = 0;
    for (int k = 0; k < 5; ++k) {
        memset(sum, 0, cells * sizeof(uint32_t));
        for (uint32_t by = 0; by < nby; ++by) {
            for (uint32_t bx = 0; bx < nbx; ++bx) {
                const uint8_t* blk = bc + (size_t)(ay[k] / 4 + by) * row + (size_t)(ax[k] / 4 + bx) * bb;
                uint8_t t[64];
                decode_blk(codec, blk, t);
                for (uint32_t i = 0; i < 16; ++i) {
                    uint32_t tx = bx * 4 + (i & 3);
                    uint32_t ty = by * 4 + (i >> 2);
                    uint32_t* cell = &sum[((size_t)(ty / fy) * pw + tx / fx) * nch];
                    for (uint32_t ch = 0; ch < nch; ++ch) {
                        uint8_t v = t[i * 4 + ch];
                        cell[ch] += v;
                        g_sum[ch] += v;
                        g_ss[ch] += (double)v * v;
                    }
                    g_n += 1.0;
                }
            }
        }
        uint8_t* out = out_px + k * cells;
        for (size_t i = 0; i < cells; ++i) out[i] = (uint8_t)nearbyint(sum[i] / per);
    }
    for (int ch = 0; ch < 4; ++ch) {
        double m = g_n > 0 ? g_sum[ch] / g_n : 0.0;
        double var = g_n > 0 ? g_ss[ch] / g_n - m * m : 0.0;
        out_stats[ch] = m;
        out_stats[4 + ch] = sqrt(var > 0 ? var : 0.0);
    }
    return 1;
}
"""


@dataclass
class DdsVisualSig:
    codec: int
    nch: int
    pw: int
    ph: int
    px: bytes
    rgb_mean: Tuple[float, float, float]
    rgb_std: Tuple[float, float, float]

    @property
    def is_flat(self) -> bool:
        return max(self.rgb_std) < 3.0


class BcVisualDecoder:
    def __init__(self, enabled: bool = True) -> None:
        self._lib = None
        self.available = False
        self.reason = "disabled"
        if not enabled:
            return
        bcdec_dir = self._find_bcdec_dir()
        compiler = shutil.which("cc") or shutil.which("gcc") or shutil.which("clang")
        if bcdec_dir is None:
            self.reason = "bcdec.h not found"
            return
        if compiler is None:
            self.reason = "no C compiler found"
            return
        cache_dir = Path(os.environ.get("XDG_CACHE_HOME", Path.home() / ".cache")) / "wwmi-dx12-rt"
        try:
            cache_dir.mkdir(parents=True, exist_ok=True)
        except OSError:
            cache_dir = Path(tempfile.gettempdir()) / "wwmi-dx12-rt-cache"
            cache_dir.mkdir(parents=True, exist_ok=True)
        src_hash = hashlib.sha256(_BCDEC_HELPER_C.encode("utf-8")).hexdigest()[:12]
        so_path = cache_dir / f"bcdec_sig_{src_hash}.so"
        if not so_path.is_file():
            with tempfile.TemporaryDirectory() as td:
                c_path = Path(td) / "bcdec_sig.c"
                tmp_so = cache_dir / f"bcdec_sig_{src_hash}.{os.getpid()}.tmp.so"
                c_path.write_text(_BCDEC_HELPER_C, encoding="utf-8")
                res = subprocess.run(
                    [compiler, "-O2", "-shared", "-fPIC", f"-I{bcdec_dir}", str(c_path), "-lm", "-o", str(tmp_so)],
                    capture_output=True,
                    text=True,
                )
                if res.returncode != 0:
                    self.reason = f"compiler error: {res.stderr.strip()[:200]}"
                    return
                tmp_so.replace(so_path)
        try:
            self._lib = ctypes.CDLL(str(so_path))
            self.available = True
            self.reason = "ok"
        except OSError as exc:
            self.reason = str(exc)

    @staticmethod
    def _find_bcdec_dir() -> Optional[Path]:
        candidates = [
            THIS_DIR / "third_party",
            THIS_DIR.parent / "third_party/wwmi-dx12/third_party/bcdec",
        ]
        for c in candidates:
            if (c / "bcdec.h").is_file():
                return c
        return None

    def compute(self, dds_path: Path, meta: DdsHeaderMeta) -> Optional[DdsVisualSig]:
        if not self.available or self._lib is None:
            return None
        codec, nch = codec_of_dxgi(meta.dxgi_format)
        if not codec or meta.width < 128 or meta.height < 128:
            return None
        if (meta.width & (meta.width - 1)) or (meta.height & (meta.height - 1)):
            return None
        bb = 8 if codec in (1, 4) else 16
        mip0_bytes = (meta.width // 4) * (meta.height // 4) * bb
        with dds_path.open("rb") as f:
            f.seek(meta.data_offset)
            bc = f.read(mip0_bytes)
        if len(bc) < mip0_bytes:
            return None
        pw = ctypes.c_uint32(0)
        ph = ctypes.c_uint32(0)
        px = (ctypes.c_uint8 * (tl.K_PATCHES * 32 * 32 * 4))()
        stats = (ctypes.c_double * 8)()
        ok = self._lib.wwmi_dds_look_sig(
            ctypes.c_uint8(codec),
            ctypes.c_uint8(nch),
            ctypes.c_uint32(meta.width),
            ctypes.c_uint32(meta.height),
            bc,
            ctypes.c_size_t(len(bc)),
            ctypes.byref(pw),
            ctypes.byref(ph),
            px,
            stats,
        )
        if not ok:
            return None
        px_len = tl.K_PATCHES * pw.value * ph.value * nch
        return DdsVisualSig(
            codec=codec,
            nch=nch,
            pw=pw.value,
            ph=ph.value,
            px=bytes(px[:px_len]),
            rgb_mean=(stats[0], stats[1], stats[2]),
            rgb_std=(stats[4], stats[5], stats[6]),
        )


def compare_sig_to_ref(sig: DdsVisualSig, ref: tl.LookRef) -> Tuple[float, float]:
    """Return (rgb_ssim, rgb_hist_sim) between a DDS 5-patch signature and a tex_look.bin LookRef."""
    pw = min(sig.pw, ref.pw)
    ph = min(sig.ph, ref.ph)
    nch = min(sig.nch, ref.nch)
    if pw == 0 or ph == 0 or nch == 0:
        return (-1.0, 0.0)

    def downsample(px: bytes, ow: int, oh: int, onch: int) -> List[float]:
        fx = ow // pw
        fy = oh // ph
        if fx == 1 and fy == 1 and onch == nch:
            return [float(v) for v in px]
        out = [0.0] * (tl.K_PATCHES * pw * ph * nch)
        for k in range(tl.K_PATCHES):
            for y in range(oh):
                dy = y // fy
                for x in range(ow):
                    dx = x // fx
                    src_i = (((k * oh + y) * ow + x) * onch)
                    dst_i = (((k * ph + dy) * pw + dx) * nch)
                    for c in range(nch):
                        out[dst_i + c] += px[src_i + c]
        inv = 1.0 / float(fx * fy)
        return [v * inv for v in out]

    va = downsample(sig.px, sig.pw, sig.ph, sig.nch)
    vb = downsample(ref.px, ref.pw, ref.ph, ref.nch)
    n = tl.K_PATCHES * pw * ph
    c1 = (0.01 * 255.0) ** 2
    c2 = (0.03 * 255.0) ** 2

    ssims: List[float] = []
    hists: List[float] = []
    for c in range(min(3, nch)):
        ha = [0] * 32
        hb = [0] * 32
        sa = sb = saa = sbb = sab = 0.0
        for i in range(n):
            a = va[i * nch + c]
            b = vb[i * nch + c]
            ha[min(31, int(a) >> 3)] += 1
            hb[min(31, int(b) >> 3)] += 1
            sa += a
            sb += b
            saa += a * a
            sbb += b * b
            sab += a * b
        ma = sa / n
        mb = sb / n
        vaa = max(0.0, saa / n - ma * ma)
        vbb = max(0.0, sbb / n - mb * mb)
        if vaa < 1.0 and vbb < 1.0:
            continue
        cov = sab / n - ma * mb
        ssim = ((2.0 * ma * mb + c1) * (2.0 * cov + c2)) / ((ma * ma + mb * mb + c1) * (vaa + vbb + c2))
        ssims.append(ssim)
        hists.append(sum(min(ha[j], hb[j]) for j in range(32)) / float(n))

    if not ssims:
        return (-1.0, 0.0)
    return (sum(ssims) / len(ssims), sum(hists) / len(hists))


def compare_two_sigs(a: DdsVisualSig, b: DdsVisualSig) -> Tuple[float, float]:
    """Compare two DDS 5-patch signatures (used for twin-variant consistency check)."""
    ref_b = tl.LookRef(target=0, codec=b.codec, nch=b.nch, w=b.pw * 16, h=b.ph * 16, pw=b.pw, ph=b.ph, px=b.px)
    return compare_sig_to_ref(a, ref_b)


# ---- Full WWMI mod.ini parser for migration ----
@dataclass
class ModResourceTex:
    name: str
    filename: str
    abs_path: Path
    components: List[int]
    file_hash: int
    dds: Optional[DdsHeaderMeta]
    mtime: float = 0.0
    visual: Optional[DdsVisualSig] = None
    override_hashes: Set[int] = field(default_factory=set)

    @property
    def exclusive(self) -> bool:
        return len(self.components) == 1


@dataclass
class ModComponentBlock:
    index: int
    section_name: str
    start_line: int
    end_line: int  # exclusive
    ib_hash: int = 0
    first_index: int = 0
    index_count: int = 0
    explicit_ps_slots: Set[int] = field(default_factory=set)
    has_backup_call: bool = False
    has_restore_call: bool = False


@dataclass
class ParsedModIni:
    path: Path
    lines: List[str]
    object_guid: int
    components: Dict[int, ModComponentBlock]
    resources: Dict[str, ModResourceTex]  # lower(name) -> ModResourceTex
    hash_to_resource: Dict[int, str]      # tex override hash -> canonical resource name
    has_slot_format_globals: bool
    has_backup_section: bool
    has_restore_section: bool
    constants_end_line: int
    legacy_tex_override_ranges: List[Tuple[int, int]]


_RE_SEC = re.compile(r"^\s*\[([^\]]+)\]\s*$")
_RE_COMP = re.compile(r"^TextureOverrideComponent(\d+)$", re.I)
_RE_COMPS_FILE = re.compile(r"components-([0-9]+(?:-[0-9]+)*)", re.I)
_RE_COMP_SINGLE = re.compile(r"component[-_]?(\d+)", re.I)
_RE_FILE_THASH = re.compile(r"(?:^|[^0-9a-zA-Z])t=([0-9a-fA-F]{8})(?:[^0-9a-fA-F]|$)")
_RE_EXPLICIT_PST = re.compile(r"^\s*ps-t(\d+)\s*=", re.I)


def parse_components_from_filename(filename: str) -> List[int]:
    base = Path(filename.replace("\\", "/")).name
    m = _RE_COMPS_FILE.search(base)
    if m:
        out: List[int] = []
        for tok in m.group(1).split("-"):
            val = int(tok)
            if val not in out:
                out.append(val)
        return out
    m2 = _RE_COMP_SINGLE.search(base)
    if m2:
        return [int(m2.group(1))]
    return []


def parse_hash_from_filename(filename: str) -> int:
    base = Path(filename.replace("\\", "/")).name
    m = _RE_FILE_THASH.search(base)
    return int(m.group(1), 16) if m else 0


def parse_mod_ini_full(mod_dir: Path, ini_path: Path) -> ParsedModIni:
    lines = ini_path.read_text(encoding="utf-8", errors="replace").splitlines()
    # Split into section blocks (name, start, end)
    sections: List[Tuple[str, int, int]] = []
    cur_name: Optional[str] = None
    cur_start = 0
    for idx, raw in enumerate(lines):
        m = _RE_SEC.match(raw)
        if m:
            if cur_name is not None:
                sections.append((cur_name, cur_start, idx))
            cur_name = m.group(1).strip()
            cur_start = idx
    if cur_name is not None:
        sections.append((cur_name, cur_start, len(lines)))

    object_guid = 0
    has_slot_globals = False
    has_backup = False
    has_restore = False
    constants_end = 0
    components: Dict[int, ModComponentBlock] = {}
    resources: Dict[str, ModResourceTex] = {}
    hash_to_resource: Dict[int, str] = {}
    legacy_ranges: List[Tuple[int, int]] = []

    for sec_name, s0, s1 in sections:
        sec_low = sec_name.lower()
        if sec_low == "constants":
            constants_end = s1
            for raw in lines[s0 + 1 : s1]:
                code = raw.split(";", 1)[0].strip()
                mg = re.match(r"^(?:global\s+)?\$object_guid\s*=\s*(\d+)", code, re.I)
                if mg:
                    object_guid = int(mg.group(1))
                if "$slot_original_format_t0" in code.lower():
                    has_slot_globals = True
        elif sec_low == "commandlistbackuppixelshaderresources":
            has_backup = True
        elif sec_low == "commandlistrestorepixelshaderresources":
            has_restore = True
        elif sec_low.startswith("resourcetexture"):
            filename = ""
            for raw in lines[s0 + 1 : s1]:
                code = raw.split(";", 1)[0].strip()
                mf = re.match(r"^filename\s*=\s*(.+)$", code, re.I)
                if mf:
                    filename = mf.group(1).strip()
            if filename:
                abs_path = mod_dir / filename.replace("\\", "/")
                dds = read_dds_header(abs_path)
                mtime = abs_path.stat().st_mtime if abs_path.is_file() else 0.0
                resources[sec_low] = ModResourceTex(
                    name=sec_name,
                    filename=filename,
                    abs_path=abs_path,
                    components=parse_components_from_filename(filename),
                    file_hash=parse_hash_from_filename(filename),
                    dds=dds,
                    mtime=mtime,
                )
        elif _RE_COMP.match(sec_name):
            cidx = int(_RE_COMP.match(sec_name).group(1))  # type: ignore[union-attr]
            blk = ModComponentBlock(index=cidx, section_name=sec_name, start_line=s0, end_line=s1)
            for raw in lines[s0 + 1 : s1]:
                code = raw.split(";", 1)[0].strip()
                if not code:
                    continue
                mp = _RE_EXPLICIT_PST.match(code)
                if mp:
                    blk.explicit_ps_slots.add(int(mp.group(1)))
                if "commandlistbackuppixelshaderresources" in code.lower():
                    blk.has_backup_call = True
                if "commandlistrestorepixelshaderresources" in code.lower():
                    blk.has_restore_call = True
                if "=" in code:
                    k, v = [p.strip() for p in code.split("=", 1)]
                    kl = k.lower()
                    if kl == "hash":
                        try:
                            blk.ib_hash = int(v, 16)
                        except ValueError:
                            pass
                    elif kl == "match_first_index":
                        try:
                            blk.first_index = int(v)
                        except ValueError:
                            pass
                    elif kl == "match_index_count":
                        try:
                            blk.index_count = int(v)
                        except ValueError:
                            pass
            components[cidx] = blk

    # Second pass: link [TextureOverrideTexture*] hashes -> ResourceTexture*
    for sec_name, s0, s1 in sections:
        if not sec_name.lower().startswith("textureoverridetexture"):
            continue
        legacy_ranges.append((s0, s1))
        ov_hash = 0
        this_targets: List[str] = []
        for raw in lines[s0 + 1 : s1]:
            code = raw.split(";", 1)[0].strip()
            if "=" not in code:
                continue
            k, v = [p.strip() for p in code.split("=", 1)]
            kl = k.lower()
            if kl == "hash":
                try:
                    ov_hash = int(v, 16)
                except ValueError:
                    pass
            elif kl == "this":
                this_targets.append(v)
        if ov_hash and this_targets:
            res_low = this_targets[0].lower()
            if res_low in resources:
                hash_to_resource[ov_hash] = resources[res_low].name
                resources[res_low].override_hashes.add(ov_hash)

    return ParsedModIni(
        path=ini_path,
        lines=lines,
        object_guid=object_guid,
        components=components,
        resources=resources,
        hash_to_resource=hash_to_resource,
        has_slot_format_globals=has_slot_globals,
        has_backup_section=has_backup,
        has_restore_section=has_restore,
        constants_end_line=constants_end,
        legacy_tex_override_ranges=legacy_ranges,
    )


# ---- Exact maximum-weight bipartite assignment for small component groups ----
def solve_max_weight_assignment(
    n_left: int,
    n_right: int,
    edges: Dict[Tuple[int, int], float],
) -> Dict[int, int]:
    """Exact maximum-weight 1-to-1 matching (left -> right) using branch-and-bound."""
    if n_left == 0 or n_right == 0 or not edges:
        return {}
    adj: List[List[Tuple[int, float]]] = [[] for _ in range(n_left)]
    for (i, j), w in edges.items():
        if w > 0.0:
            adj[i].append((j, w))
    for i in range(n_left):
        adj[i].sort(key=lambda x: -x[1])

    # Precompute upper bound from remaining left nodes
    max_per_left = [adj[i][0][1] if adj[i] else 0.0 for i in range(n_left)]
    suffix_ub = [0.0] * (n_left + 1)
    for i in range(n_left - 1, -1, -1):
        suffix_ub[i] = suffix_ub[i + 1] + max_per_left[i]

    best_score = -1.0
    best_assign: Dict[int, int] = {}
    cur_assign: Dict[int, int] = {}
    used_right: Set[int] = set()

    def dfs(i: int, score: float) -> None:
        nonlocal best_score, best_assign
        if score + suffix_ub[i] <= best_score + 1e-12:
            return
        if i == n_left:
            if score > best_score:
                best_score = score
                best_assign = dict(cur_assign)
            return
        # Try top candidates for node i (limit fanout to 10 to stay instant even on huge mods)
        for j, w in adj[i][:10]:
            if j in used_right:
                continue
            used_right.add(j)
            cur_assign[i] = j
            dfs(i + 1, score + w)
            del cur_assign[i]
            used_right.remove(j)
        # Option to leave node i unmatched
        dfs(i + 1, score)

    dfs(0, 0.0)
    return best_assign


# ---- Core generic migration solver ----
@dataclass
class LogicalSlotGroup:
    comp: int
    key: str
    vfmt: int
    lod_target: int
    slots: List[Tuple[int, int]]  # (slot_index, vfmt)
    max_res: Tuple[int, int]
    live_hashes: List[int]
    anchor_rule: int = 0


@dataclass
class CandidateScore:
    resource_name: str
    filename: str
    dxgi_format: int
    exclusive: bool
    exact_format_match: bool
    ssim: Optional[float]
    hist_sim: Optional[float]
    combined: float
    is_flat: bool = False


@dataclass
class ResolvedBinding:
    comp: int
    logical_key: str
    slots: List[Tuple[int, int]]  # (slot_index, vfmt)
    vfmt: int
    lod_target: int
    selected_resource: Optional[str]
    selected_filename: Optional[str]
    confidence: str  # ANCHOR_EXACT, HIGH_UNIQUE_FORMAT, HIGH_VISUAL, MEDIUM_VISUAL, AMBIGUOUS, UNMATCHED
    enabled: bool
    reason: str
    candidates: List[CandidateScore] = field(default_factory=list)


def plan_hash_free_migration(
    mod: ParsedModIni,
    audit_rows: Sequence[tl.AuditRow],
    *,
    texlook: Optional[tl.TexLookData],
    lodcache: Optional[tl.LodCacheData],
    decoder: BcVisualDecoder,
    guid_filter: Optional[int] = None,
    min_confidence: str = "medium",
    mapping_override: Optional[Dict[str, object]] = None,
) -> Tuple[List[ResolvedBinding], Dict[str, object]]:
    guid = guid_filter if guid_filter is not None else (mod.object_guid or None)
    filtered = [r for r in audit_rows if guid is None or r.guid == guid]

    # Detect generic engine/shared noise textures (same hash across >=4 comps at <=256x256 without lod/rule)
    hash_comps: Dict[int, Set[int]] = defaultdict(set)
    hash_max_dim: Dict[int, int] = defaultdict(int)
    hash_has_target: Dict[int, bool] = defaultdict(bool)
    for r in filtered:
        if r.tex_hash:
            hash_comps[r.tex_hash].add(r.comp)
            hash_max_dim[r.tex_hash] = max(hash_max_dim[r.tex_hash], r.width, r.height)
            if r.lod or r.rule:
                hash_has_target[r.tex_hash] = True
    shared_engine_hashes = {
        h for h, cs in hash_comps.items() if len(cs) >= 4 and hash_max_dim[h] <= 256 and not hash_has_target[h]
    }

    lod_index = lodcache.records if lodcache is not None else {}

    # 1) Group observed valid texture slots by (comp, slot, vfmt)
    slot_obs: Dict[Tuple[int, int, int], List[tl.AuditRow]] = defaultdict(list)
    for r in filtered:
        comp_blk = mod.components.get(r.comp)
        if comp_blk is not None and r.slot in comp_blk.explicit_ps_slots:
            # Respect explicit author ps-tN = ... overrides (e.g., ps-t4 = null)
            continue
        if r.width < 256 or r.height < 256 or r.width != r.height or r.vfmt == 0:
            continue
        if r.tex_hash in shared_engine_hashes and not r.rule and not r.lod and not r.ovr:
            continue
        slot_obs[(r.comp, r.slot, r.vfmt)].append(r)

    # 2) Build LogicalSlotGroups per component, coalescing slots that share the same canonical lod target
    by_comp_groups: Dict[int, List[LogicalSlotGroup]] = defaultdict(list)
    anchor_locked_resources: Set[str] = set()  # lower(resource_name)

    for comp in sorted({k[0] for k in slot_obs.keys()}):
        temp_groups: Dict[str, LogicalSlotGroup] = {}
        for (_, slot, vfmt), rows in sorted(slot_obs.items()):
            if _ != comp:
                continue
            live_hashes = sorted({r.tex_hash for r in rows if r.tex_hash})
            rules = sorted({r.rule for r in rows if r.rule})
            lods = sorted({r.lod for r in rows if r.lod})
            for lh in live_hashes:
                for link in lod_index.get(lh, []):
                    if link.high not in lods:
                        lods.append(link.high)
            lods = sorted(set(lods))

            max_w = max(r.width for r in rows)
            max_h = max(r.height for r in rows)
            anchor_rule = rules[0] if rules else 0
            lod_tgt = lods[0] if lods else 0

            if anchor_rule:
                key = f"rule:{anchor_rule:08x}"
            elif lod_tgt:
                key = f"lod:{lod_tgt:08x}"
            elif len(live_hashes) == 1:
                key = f"live:{live_hashes[0]:08x}:fmt{vfmt}"
            else:
                key = f"slot:t{slot}:fmt{vfmt}"

            grp = temp_groups.get(key)
            if grp is None:
                grp = LogicalSlotGroup(
                    comp=comp,
                    key=key,
                    vfmt=vfmt,
                    lod_target=lod_tgt,
                    slots=[(slot, vfmt)],
                    max_res=(max_w, max_h),
                    live_hashes=live_hashes,
                    anchor_rule=anchor_rule,
                )
                temp_groups[key] = grp
            else:
                if (slot, vfmt) not in grp.slots:
                    grp.slots.append((slot, vfmt))
                grp.max_res = (max(grp.max_res[0], max_w), max(grp.max_res[1], max_h))
                grp.live_hashes = sorted(set(grp.live_hashes) | set(live_hashes))
                # Prefer primary non-duplicate vfmt (keep the format of the highest-res observation or canonical slot)
                if max_w > grp.max_res[0]:
                    grp.vfmt = vfmt

        by_comp_groups[comp] = list(temp_groups.values())

    # 3) Lock Layer A anchors globally so anchor resources never pollute other format buckets
    bindings: List[ResolvedBinding] = []
    anchored_keys: Set[Tuple[int, str]] = set()
    for comp, groups in sorted(by_comp_groups.items()):
        for grp in groups:
            if grp.anchor_rule and grp.anchor_rule in mod.hash_to_resource:
                res_name = mod.hash_to_resource[grp.anchor_rule]
                res_obj = mod.resources[res_name.lower()]
                anchor_locked_resources.add(res_name.lower())
                anchored_keys.add((comp, grp.key))
                bindings.append(
                    ResolvedBinding(
                        comp=comp,
                        logical_key=grp.key,
                        slots=sorted(grp.slots),
                        vfmt=grp.vfmt,
                        lod_target=grp.lod_target or grp.anchor_rule,
                        selected_resource=res_name,
                        selected_filename=res_obj.filename,
                        confidence="ANCHOR_EXACT",
                        enabled=True,
                        reason=f"Cross-patch invariant anchor hit (rule={grp.anchor_rule:08x})",
                    )
                )

    # Precompute visual signatures lazily for candidate DDS files when needed
    def ensure_visual(res: ModResourceTex) -> Optional[DdsVisualSig]:
        if res.visual is None and res.dds is not None and decoder.available:
            res.visual = decoder.compute(res.abs_path, res.dds)
        return res.visual

    # 4) Solve remaining logical groups per component using Layers B + C + D
    for comp, groups in sorted(by_comp_groups.items()):
        pending = [g for g in groups if (comp, g.key) not in anchored_keys]
        if not pending:
            continue

        # Candidate resources belonging to this component and not already locked by an invariant anchor
        comp_pool = [
            r
            for r in mod.resources.values()
            if comp in r.components
            and r.dds is not None
            and r.dds.width >= 256
            and r.dds.height >= 256
            and r.dds.width == r.dds.height
            and r.name.lower() not in anchor_locked_resources
        ]
        comp_pool.sort(key=lambda r: (0 if r.exclusive else 1, r.name.lower()))

        # Compute visual stats if decoder available
        if decoder.available and texlook is not None:
            for r in comp_pool:
                ensure_visual(r)

        # Build score matrix between pending groups and comp_pool
        group_cands: Dict[int, List[Tuple[int, CandidateScore]]] = defaultdict(list)
        edges: Dict[Tuple[int, int], float] = {}

        for gi, grp in enumerate(pending):
            ref = texlook.refs.get(grp.lod_target) if (texlook is not None and grp.lod_target) else None
            # Determine eligible candidates for this group's vfmt
            same_fmt = [ri for ri, r in enumerate(comp_pool) if r.dds and r.dds.dxgi_format == grp.vfmt]
            has_excl_same_fmt = any(comp_pool[ri].exclusive for ri in same_fmt)
            compat_indices = list(same_fmt)
            # Universal rule for BC3_UNORM (77) slots re-saved as BC7_UNORM (98) by mod authors:
            # when no exclusive BC3_UNORM exists for this component AND we have a visual Ref, also evaluate
            # non-flat exclusive BC7_UNORM candidates by 5-patch visual similarity.
            if grp.vfmt == 77 and not has_excl_same_fmt and ref is not None and decoder.available:
                for ri, r in enumerate(comp_pool):
                    if (
                        ri not in compat_indices
                        and r.exclusive
                        and r.dds
                        and r.dds.dxgi_format == 98
                        and (r.visual is None or not r.visual.is_flat)
                    ):
                        compat_indices.append(ri)

            for ri in compat_indices:
                r = comp_pool[ri]
                assert r.dds is not None
                exact_fmt = r.dds.dxgi_format == grp.vfmt
                ssim_v: Optional[float] = None
                hist_v: Optional[float] = None
                is_flat = bool(r.visual and r.visual.is_flat)

                if ref is not None and r.visual is not None:
                    if is_flat:
                        # Flat/black texture (e.g. re-saved empty mask) must not hijack a structured BC slot
                        combined = 0.0
                    else:
                        ssim_v, hist_v = compare_sig_to_ref(r.visual, ref)
                        combined = 0.60 * max(0.0, ssim_v) + 0.40 * hist_v
                        if exact_fmt:
                            combined += 0.05
                        if r.exclusive:
                            combined += 0.03
                else:
                    # Pure structural mode (no ref or no decoder): only score exclusive exact-format matches
                    combined = 1.0 if (exact_fmt and r.exclusive) else 0.0

                cs = CandidateScore(
                    resource_name=r.name,
                    filename=r.filename,
                    dxgi_format=r.dds.dxgi_format,
                    exclusive=r.exclusive,
                    exact_format_match=exact_fmt,
                    ssim=round(ssim_v, 4) if ssim_v is not None else None,
                    hist_sim=round(hist_v, 4) if hist_v is not None else None,
                    combined=round(combined, 4),
                    is_flat=is_flat,
                )
                group_cands[gi].append((ri, cs))
                if combined > 0.0:
                    edges[(gi, ri)] = combined

            group_cands[gi].sort(key=lambda item: (-item[1].combined, item[1].resource_name.lower()))

        # Separate groups that have a visual Ref vs pure-structural groups
        visual_edge_map: Dict[Tuple[int, int], float] = {}
        for gi, grp in enumerate(pending):
            ref = texlook.refs.get(grp.lod_target) if (texlook is not None and grp.lod_target) else None
            if ref is not None and decoder.available:
                for ri, cs in group_cands[gi]:
                    if cs.combined > 0.0:
                        visual_edge_map[(gi, ri)] = cs.combined

        # Exact max-weight 1-to-1 assignment over visually-scored groups first
        assigned = solve_max_weight_assignment(len(pending), len(comp_pool), visual_edge_map)
        used_resources: Set[int] = set(assigned.values())

        for gi, grp in enumerate(pending):
            ranked = [cs for _, cs in group_cands[gi]]
            ref = texlook.refs.get(grp.lod_target) if (texlook is not None and grp.lod_target) else None
            excl_same_all = [
                (ri, cs)
                for ri, cs in group_cands[gi]
                if cs.exclusive and cs.exact_format_match
            ]
            excl_same = [(ri, cs) for ri, cs in excl_same_all if not cs.is_flat]

            chosen_ri = assigned.get(gi)
            if chosen_ri is not None:
                chosen_res = comp_pool[chosen_ri]
                chosen_cs = next(cs for ri, cs in group_cands[gi] if ri == chosen_ri)
                others = [(ri, cs) for ri, cs in group_cands[gi] if ri != chosen_ri and not cs.is_flat]
                runner_up = others[0][1] if others else None
                margin = chosen_cs.combined - (runner_up.combined if runner_up else 0.0)
                ssim_val = chosen_cs.ssim if chosen_cs.ssim is not None else -1.0
                hist_val = chosen_cs.hist_sim if chosen_cs.hist_sim is not None else 0.0

                # Twin-candidate consistency check: if top candidates are near-identical variants
                # (e.g., base/burst form twins of the same texture slot), measure margin against the
                # highest-scoring NON-twin candidate instead of falsely flagging the twin pair ambiguous.
                twin_pair = False
                non_twin_margin = margin
                if chosen_res.visual is not None and others and margin < 0.05:
                    first_non_twin_score = 0.0
                    saw_twin = False
                    for ori, ocs in others:
                        ores = comp_pool[ori]
                        if ores.visual is not None:
                            t_ssim, t_hist = compare_two_sigs(chosen_res.visual, ores.visual)
                            if t_ssim >= 0.85 or t_hist >= 0.90:
                                saw_twin = True
                                continue
                        first_non_twin_score = ocs.combined
                        break
                    if saw_twin:
                        twin_pair = True
                        non_twin_margin = chosen_cs.combined - first_non_twin_score

                if ssim_val >= 0.75:
                    conf = "HIGH_VISUAL"
                    reason = f"Strong 5-patch visual match to canonical HD ref {grp.lod_target:08x} (ssim={ssim_val:.3f}, hist={hist_val:.2f})"
                elif len(excl_same_all) == 1 and excl_same_all[0][0] == chosen_ri and chosen_cs.combined >= 0.20:
                    conf = "HIGH_UNIQUE_FORMAT"
                    reason = f"Unambiguous single exclusive {tl.dxgi_name(grp.vfmt)} texture for comp={comp} corroborated visually (ssim={ssim_val:.3f}, hist={hist_val:.2f})"
                elif chosen_cs.combined >= 0.45 and margin >= 0.08:
                    conf = "HIGH_VISUAL"
                    reason = f"Clear visual leader for ref {grp.lod_target:08x} (score={chosen_cs.combined:.3f}, margin=+{margin:.3f}, ssim={ssim_val:.3f}, hist={hist_val:.2f})"
                elif twin_pair and chosen_cs.combined >= 0.12 and non_twin_margin >= 0.04:
                    conf = "MEDIUM_VISUAL"
                    reason = (
                        f"Twin-variant pair visually matched to ref {grp.lod_target:08x} "
                        f"(score={chosen_cs.combined:.3f}, non-twin margin=+{non_twin_margin:.3f}, ssim={ssim_val:.3f}, hist={hist_val:.2f})"
                    )
                elif chosen_cs.combined >= 0.14 and (
                    margin >= 0.025
                    or (len(excl_same) == 2 and chosen_cs.exact_format_match and chosen_cs.exclusive)
                    or (not chosen_cs.exact_format_match and hist_val >= 0.20 and margin >= 0.05)
                ):
                    conf = "MEDIUM_VISUAL"
                    reason = (
                        f"Best visual candidate for ref {grp.lod_target:08x} among {len(ranked)} candidate(s) "
                        f"(score={chosen_cs.combined:.3f}, margin=+{margin:.3f}, ssim={ssim_val:.3f}, hist={hist_val:.2f})"
                    )
                elif len(excl_same_all) == 1 and excl_same_all[0][0] == chosen_ri:
                    conf = "HIGH_UNIQUE_FORMAT"
                    reason = f"Unambiguous single exclusive {tl.dxgi_name(grp.vfmt)} texture for comp={comp}"
                else:
                    conf = "AMBIGUOUS"
                    reason = (
                        f"Multiple similar candidates for ref {grp.lod_target:08x} "
                        f"(top={chosen_res.name} score={chosen_cs.combined:.3f}, margin=+{margin:.3f}); manual review required"
                    )

                enabled = (
                    conf in ("ANCHOR_EXACT", "HIGH_UNIQUE_FORMAT", "HIGH_VISUAL")
                    or (min_confidence == "medium" and conf == "MEDIUM_VISUAL")
                )
                bindings.append(
                    ResolvedBinding(
                        comp=comp,
                        logical_key=grp.key,
                        slots=sorted(grp.slots),
                        vfmt=grp.vfmt,
                        lod_target=grp.lod_target,
                        selected_resource=chosen_res.name if conf != "AMBIGUOUS" else None,
                        selected_filename=chosen_res.filename if conf != "AMBIGUOUS" else None,
                        confidence=conf,
                        enabled=enabled,
                        reason=reason,
                        candidates=ranked[:6],
                    )
                )
                continue

            # Pure structural fallback (no visual Ref available or decoder disabled).
            # Flat same-format textures (e.g., smooth normal maps) remain eligible when unique.
            unused_excl_same = [(ri, cs) for ri, cs in excl_same_all if ri not in used_resources]
            if len(unused_excl_same) == 1:
                ri, cs = unused_excl_same[0]
                used_resources.add(ri)
                chosen_res = comp_pool[ri]
                bindings.append(
                    ResolvedBinding(
                        comp=comp,
                        logical_key=grp.key,
                        slots=sorted(grp.slots),
                        vfmt=grp.vfmt,
                        lod_target=grp.lod_target,
                        selected_resource=chosen_res.name,
                        selected_filename=chosen_res.filename,
                        confidence="HIGH_UNIQUE_FORMAT",
                        enabled=True,
                        reason=f"Unambiguous single exclusive {tl.dxgi_name(grp.vfmt)} texture for comp={comp}",
                        candidates=ranked[:6],
                    )
                )
            elif len(unused_excl_same) > 1:
                bindings.append(
                    ResolvedBinding(
                        comp=comp,
                        logical_key=grp.key,
                        slots=sorted(grp.slots),
                        vfmt=grp.vfmt,
                        lod_target=grp.lod_target,
                        selected_resource=None,
                        selected_filename=None,
                        confidence="AMBIGUOUS",
                        enabled=False,
                        reason=f"{len(unused_excl_same)} exclusive {tl.dxgi_name(grp.vfmt)} candidates without visual ref; skipped to prevent cross-texture",
                        candidates=ranked[:6],
                    )
                )
            else:
                bindings.append(
                    ResolvedBinding(
                        comp=comp,
                        logical_key=grp.key,
                        slots=sorted(grp.slots),
                        vfmt=grp.vfmt,
                        lod_target=grp.lod_target,
                        selected_resource=None,
                        selected_filename=None,
                        confidence="UNMATCHED",
                        enabled=False,
                        reason=f"No matching exclusive {tl.dxgi_name(grp.vfmt)} mod texture for comp={comp}",
                        candidates=ranked[:6],
                    )
                )

    # 5) Apply optional user --mapping-override edits
    if mapping_override and isinstance(mapping_override.get("bindings"), list):
        ov_map: Dict[Tuple[int, str], Dict[str, object]] = {}
        for item in mapping_override["bindings"]:
            if isinstance(item, dict) and "comp" in item and "logical_key" in item:
                ov_map[(int(item["comp"]), str(item["logical_key"]))] = item
        for b in bindings:
            ov = ov_map.get((b.comp, b.logical_key))
            if not ov:
                continue
            if "selected_resource" in ov:
                sel = ov["selected_resource"]
                if sel:
                    sel_str = str(sel)
                    res_obj = mod.resources.get(sel_str.lower())
                    b.selected_resource = res_obj.name if res_obj else sel_str
                    b.selected_filename = res_obj.filename if res_obj else b.selected_filename
                else:
                    b.selected_resource = None
                    b.selected_filename = None
            if "enabled" in ov:
                b.enabled = bool(ov["enabled"]) and bool(b.selected_resource)
            b.reason += " [user-override]"

    bindings.sort(key=lambda b: (b.comp, b.slots[0][0] if b.slots else 99, b.vfmt, b.logical_key))

    summary_counts: Dict[str, int] = defaultdict(int)
    for b in bindings:
        summary_counts[b.confidence] += 1
    meta_out = {
        "guid": guid,
        "mod_ini": str(mod.path),
        "visual_decoder_available": decoder.available,
        "visual_decoder_status": decoder.reason,
        "shared_engine_hashes": [f"{h:08x}" for h in sorted(shared_engine_hashes)],
        "confidence_summary": dict(sorted(summary_counts.items())),
        "enabled_bindings": sum(1 for b in bindings if b.enabled and b.selected_resource),
        "total_logical_groups": len(bindings),
    }
    return bindings, meta_out


# ---- Standard WWMI Hash-Free INI generator ----
def generate_hash_free_ini(
    mod: ParsedModIni,
    bindings: Sequence[ResolvedBinding],
    *,
    comment_legacy_tex_overrides: bool = False,
) -> Tuple[str, str]:
    """Return (full_hash_free_ini, delta_snippet_ini)."""
    # Group enabled (slot, vfmt) -> resource per component, detecting any (comp, slot, vfmt) conflict
    comp_rules: Dict[int, Dict[Tuple[int, int], List[ResolvedBinding]]] = defaultdict(lambda: defaultdict(list))
    comp_commented: Dict[int, List[ResolvedBinding]] = defaultdict(list)

    for b in bindings:
        if b.enabled and b.selected_resource:
            for slot_idx, vfmt in b.slots:
                comp_rules[b.comp][(slot_idx, vfmt)].append(b)
        else:
            comp_commented[b.comp].append(b)

    # Determine which slots each component needs to sample via ps-tK->Format
    comp_needed_slots: Dict[int, List[int]] = {}
    comp_cmdlist_blocks: Dict[int, List[str]] = {}

    for comp in sorted(set(comp_rules.keys()) | set(comp_commented.keys())):
        slot_entries = comp_rules.get(comp, {})
        needed_slots: Set[int] = set()
        cmd_lines = [
            f"[CommandListSetTexturesComponent{comp}]",
            "if $object_detected == 1",
        ]
        active_count = 0
        for (slot_idx, vfmt), blist in sorted(slot_entries.items()):
            unique_res = sorted({b.selected_resource for b in blist if b.selected_resource})
            if len(unique_res) != 1:
                cmd_lines.append(
                    f"    ; [CONFLICT SKIPPED] ps-t{slot_idx} ({tl.dxgi_name(vfmt)}) had multiple targets: {', '.join(unique_res)}"
                )
                continue
            needed_slots.add(slot_idx)
            b0 = blist[0]
            cmd_lines.append(
                f"    ; {b0.confidence}: {b0.selected_filename or unique_res[0]} (key={b0.logical_key})"
            )
            cmd_lines.append(f"    if $slot_original_format_t{slot_idx} == {tl.dxgi_name(vfmt)}")
            cmd_lines.append(f"        ps-t{slot_idx} = ref {unique_res[0]}")
            cmd_lines.append("    endif")
            active_count += 1

        for cb in comp_commented.get(comp, []):
            slots_s = ",".join(f"t{s}({tl.dxgi_name(vf)})" for s, vf in cb.slots)
            top_cands = ", ".join(f"{c.resource_name}({c.combined:.2f})" for c in cb.candidates[:3]) or "none"
            cmd_lines.append(f"    ; [{cb.confidence}] {slots_s} | {cb.reason} | candidates: {top_cands}")

        cmd_lines.append("endif")
        if active_count > 0:
            comp_needed_slots[comp] = sorted(needed_slots)
            comp_cmdlist_blocks[comp] = cmd_lines

    # Build standalone delta snippet
    snippet_lines: List[str] = [
        "; ============================================================",
        "; wwmi-dx12-rt Route 3 Hash-Free Texture Layer (Auto-Generated)",
        "; Cross-quality (HD / 4K UHD) & cross-patch resilient ps-tN->Format bindings.",
        "; ============================================================",
        "",
    ]
    if not mod.has_slot_format_globals:
        snippet_lines.append("; Add to [Constants]:")
        for s in range(9):
            snippet_lines.append(f"global $slot_original_format_t{s} = 0")
        snippet_lines.append("")

    for comp in sorted(comp_cmdlist_blocks.keys()):
        snippet_lines.extend(comp_cmdlist_blocks[comp])
        snippet_lines.append("")

    if not mod.has_backup_section:
        snippet_lines.extend(
            [
                "[CommandListBackupPixelShaderResources]",
                *[f"ResourceBypassPST{s} = ref ps-t{s}" for s in range(9)],
                "",
            ]
        )
    if not mod.has_restore_section:
        snippet_lines.extend(
            [
                "[CommandListRestorePixelShaderResources]",
                *[f"ps-t{s} = ref ResourceBypassPST{s}" for s in range(9)],
                "",
                *[f"[ResourceBypassPST{s}]\n" for s in range(9)],
            ]
        )
    snippet_text = "\n".join(snippet_lines).rstrip() + "\n"

    # Assemble full non-destructive mod_hash_free.ini
    legacy_line_set: Set[int] = set()
    if comment_legacy_tex_overrides:
        for s0, s1 in mod.legacy_tex_override_ranges:
            for li in range(s0, s1):
                legacy_line_set.add(li)

    # Index component blocks by line number for clean in-place surgical insertion
    comp_by_start = {blk.start_line: blk for blk in mod.components.values()}
    out_lines: List[str] = []
    i = 0
    n_lines = len(mod.lines)
    globals_inserted = mod.has_slot_format_globals

    while i < n_lines:
        # Insert $slot_original_format_t0..8 at the end of [Constants]
        if not globals_inserted and i == mod.constants_end_line and mod.constants_end_line > 0:
            out_lines.append("")
            out_lines.append("; Route 3 Hash-Free slot format globals")
            for s in range(9):
                out_lines.append(f"global $slot_original_format_t{s} = 0")
            out_lines.append("")
            globals_inserted = True

        if i in comp_by_start:
            blk = comp_by_start[i]
            sec_lines = list(mod.lines[blk.start_line : blk.end_line])
            if blk.index in comp_needed_slots:
                slots_to_read = comp_needed_slots[blk.index]
                # Find insertion point inside `if $mod_enabled`: right before CommandListTriggerResourceOverrides
                # or right after `handling = skip` / before CommandListOverrideSharedResources.
                pre_idx = None
                post_idx = None
                indent = "    "
                for li, raw in enumerate(sec_lines):
                    code = raw.split(";", 1)[0].strip().lower()
                    if pre_idx is None and (
                        code.startswith("run = commandlisttriggerresourceoverrides")
                        or code.startswith("run = commandlistoverridesharedresources")
                        or code.startswith("drawindexed")
                    ):
                        pre_idx = li
                        lead = len(raw) - len(raw.lstrip(" \t"))
                        indent = raw[:lead] if lead else "    "
                    if code.startswith("run = commandlistcleanupsharedresources"):
                        post_idx = li
                # Insert restore right after CleanupSharedResources
                if post_idx is not None and not blk.has_restore_call:
                    lead = len(sec_lines[post_idx]) - len(sec_lines[post_idx].lstrip(" \t"))
                    pindent = sec_lines[post_idx][:lead] if lead else indent
                    sec_lines.insert(post_idx + 1, f"{pindent}run = CommandListRestorePixelShaderResources")
                # Insert backup + format capture + SetTextures before OverrideSharedResources
                if pre_idx is not None:
                    inject: List[str] = []
                    if not blk.has_backup_call:
                        inject.append(f"{indent}run = CommandListBackupPixelShaderResources")
                    for s in slots_to_read:
                        inject.append(f"{indent}$slot_original_format_t{s} = ps-t{s}->Format")
                    # Keep TriggerResourceOverrides first, then SetTexturesComponentN
                    code_at_pre = sec_lines[pre_idx].split(";", 1)[0].strip().lower()
                    if code_at_pre.startswith("run = commandlisttriggerresourceoverrides"):
                        sec_lines[pre_idx : pre_idx + 1] = inject + [
                            sec_lines[pre_idx],
                            f"{indent}run = CommandListSetTexturesComponent{blk.index}",
                        ]
                    else:
                        inject.append(f"{indent}run = CommandListSetTexturesComponent{blk.index}")
                        sec_lines[pre_idx:pre_idx] = inject
            out_lines.extend(sec_lines)
            i = blk.end_line
            continue

        line = mod.lines[i]
        if i in legacy_line_set and line.strip() and not line.lstrip().startswith(";"):
            out_lines.append("; [legacy-hash-disabled] " + line)
        else:
            out_lines.append(line)
        i += 1

    if not globals_inserted:
        out_lines.insert(0, "")
        for s in reversed(range(9)):
            out_lines.insert(0, f"global $slot_original_format_t{s} = 0")
        out_lines.insert(0, "[Constants]")

    out_lines.append("")
    out_lines.append("; ============================================================")
    out_lines.append("; Route 3 Hash-Free Texture CommandLists (Appended)")
    out_lines.append("; ============================================================")
    for comp in sorted(comp_cmdlist_blocks.keys()):
        out_lines.append("")
        out_lines.extend(comp_cmdlist_blocks[comp])
    if not mod.has_backup_section:
        out_lines.append("")
        out_lines.append("[CommandListBackupPixelShaderResources]")
        out_lines.extend(f"ResourceBypassPST{s} = ref ps-t{s}" for s in range(9))
    if not mod.has_restore_section:
        out_lines.append("")
        out_lines.append("[CommandListRestorePixelShaderResources]")
        out_lines.extend(f"ps-t{s} = ref ResourceBypassPST{s}" for s in range(9))
        for s in range(9):
            out_lines.append("")
            out_lines.append(f"[ResourceBypassPST{s}]")
    out_lines.append("")
    return "\n".join(out_lines), snippet_text


def format_migration_report_md(
    bindings: Sequence[ResolvedBinding],
    meta: Dict[str, object],
) -> str:
    lines = [
        "# Route 3 Hash -> Hash-Free Migration Report",
        "",
        f"- Mod INI: `{meta['mod_ini']}`",
        f"- Object GUID: `{meta['guid']}`",
        f"- Visual Decoder (`bcdec`): `{meta['visual_decoder_available']}` ({meta['visual_decoder_status']})",
        f"- Shared Engine Hashes Filtered: `{', '.join(meta['shared_engine_hashes']) or 'none'}`",
        f"- Enabled Bindings: `{meta['enabled_bindings']}` / `{meta['total_logical_groups']}` logical groups",
        f"- Confidence Breakdown: `{json.dumps(meta['confidence_summary'], ensure_ascii=False)}`",
        "",
        "| Comp | Slots (View Format) | Canonical Ref | Confidence | Enabled | Selected Resource | Filename | Reason |",
        "|---:|---|---|---|---|---|---|---|",
    ]
    for b in bindings:
        slots_s = ", ".join(f"`t{s}` ({tl.dxgi_name(vf).replace('DXGI_FORMAT_', '')})" for s, vf in b.slots)
        ref_s = f"`{b.lod_target:08x}`" if b.lod_target else "-"
        sel_s = f"`{b.selected_resource}`" if b.selected_resource else "-"
        fn_s = f"`{Path(b.selected_filename).name}`" if b.selected_filename else "-"
        en_s = "YES" if b.enabled else "no"
        lines.append(
            f"| {b.comp} | {slots_s} | {ref_s} | `{b.confidence}` | {en_s} | {sel_s} | {fn_s} | {b.reason} |"
        )
    lines.append("")
    return "\n".join(lines)


def serialize_mapping_json(bindings: Sequence[ResolvedBinding], meta: Dict[str, object]) -> Dict[str, object]:
    return {
        "meta": meta,
        "bindings": [
            {
                "comp": b.comp,
                "logical_key": b.logical_key,
                "slots": [{"slot": s, "vfmt": vf, "vfmt_name": tl.dxgi_name(vf)} for s, vf in b.slots],
                "vfmt": b.vfmt,
                "vfmt_name": tl.dxgi_name(b.vfmt),
                "lod_target": f"{b.lod_target:08x}" if b.lod_target else None,
                "confidence": b.confidence,
                "enabled": b.enabled,
                "selected_resource": b.selected_resource,
                "selected_filename": b.selected_filename,
                "reason": b.reason,
                "candidates": [
                    {
                        "resource_name": c.resource_name,
                        "filename": c.filename,
                        "dxgi_format": c.dxgi_format,
                        "dxgi_name": tl.dxgi_name(c.dxgi_format),
                        "exclusive": c.exclusive,
                        "exact_format_match": c.exact_format_match,
                        "ssim": c.ssim,
                        "hist_sim": c.hist_sim,
                        "combined": c.combined,
                        "is_flat": c.is_flat,
                    }
                    for c in b.candidates
                ],
            }
            for b in bindings
        ],
    }


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="migrate_hash_free.py",
        description="Non-destructive offline Hash -> Hash-Free (ps-tN->Format) migration tool for WWMI mods.",
    )
    p.add_argument("--mod-dir", required=True, help="Directory containing the classic WWMI mod (mod.ini + Textures/)")
    p.add_argument("--ini-name", default="mod.ini", help="Mod INI filename inside --mod-dir (default: mod.ini)")
    p.add_argument("--audit-log", required=True, help="wwmi_dx12 log containing TEX-UHD-AUDIT lines")
    p.add_argument("--texlook", help="tex_look.bin or seed .texlook for 5-patch visual disambiguation")
    p.add_argument("--lodcache", help="lod_tex_cache.lodcache for UHD->HD canonical hash lookup")
    p.add_argument("--guid", type=int, help="Optional object_guid override for --audit-log filtering")
    p.add_argument("--out-dir", required=True, help="Output directory (original mod files are NEVER overwritten)")
    p.add_argument("--min-confidence", choices=["high", "medium"], default="medium",
                   help="Minimum confidence level auto-enabled in generated INI (default: medium)")
    p.add_argument("--mapping-override", help="Optional edited slot_mapping.json to override selected resources")
    p.add_argument("--comment-legacy-tex-overrides", action="store_true",
                   help="Comment out legacy [TextureOverrideTexture*] sections in mod_hash_free.ini")
    p.add_argument("--no-visual", action="store_true", help="Disable bcdec 5-patch visual comparison (pure structural mode)")
    return p


def main(argv: Optional[Sequence[str]] = None) -> int:
    args = build_parser().parse_args(argv)
    mod_dir = Path(args.mod_dir).resolve()
    ini_path = mod_dir / args.ini_name
    if not ini_path.is_file():
        print(f"ERROR: mod INI not found: {ini_path}", file=sys.stderr)
        return 2

    out_dir = Path(args.out_dir).resolve()
    out_dir.mkdir(parents=True, exist_ok=True)

    mod = parse_mod_ini_full(mod_dir, ini_path)
    audit_rows, _ = tl.parse_audit_log(Path(args.audit_log))
    texlook = tl.read_texlook(Path(args.texlook)) if args.texlook else None
    lodcache = tl.read_lodcache(Path(args.lodcache)) if args.lodcache else None
    override = json.loads(Path(args.mapping_override).read_text(encoding="utf-8")) if args.mapping_override else None
    decoder = BcVisualDecoder(enabled=not args.no_visual)

    bindings, meta = plan_hash_free_migration(
        mod,
        audit_rows,
        texlook=texlook,
        lodcache=lodcache,
        decoder=decoder,
        guid_filter=args.guid,
        min_confidence=args.min_confidence,
        mapping_override=override,
    )

    full_ini, snippet_ini = generate_hash_free_ini(
        mod,
        bindings,
        comment_legacy_tex_overrides=args.comment_legacy_tex_overrides,
    )
    report_md = format_migration_report_md(bindings, meta)
    mapping_obj = serialize_mapping_json(bindings, meta)

    (out_dir / "mod_hash_free.ini").write_text(full_ini, encoding="utf-8")
    (out_dir / "hash_free_snippet.ini").write_text(snippet_ini, encoding="utf-8")
    (out_dir / "migration_report.md").write_text(report_md, encoding="utf-8")
    (out_dir / "slot_mapping.json").write_text(json.dumps(mapping_obj, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")

    print(report_md)
    print(f"Wrote non-destructive outputs to: {out_dir}")
    print(f"  - {out_dir / 'mod_hash_free.ini'}")
    print(f"  - {out_dir / 'hash_free_snippet.ini'}")
    print(f"  - {out_dir / 'slot_mapping.json'}")
    print(f"  - {out_dir / 'migration_report.md'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
