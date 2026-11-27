#!/usr/bin/env python3
"""Incrementally stage upstream third_party sources + repository overlays into
<build_dir>/_staged/{reshade,wwmi-dx12,3dm_src} without touching third_party/.

Only updates destination files whose size or content differs, preserving mtimes
on unchanged files so Ninja incremental builds stay sub-second.
"""
from __future__ import annotations

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "scripts"))
from build_support import Archive, files_under, prune_tree, sync_file

_SOURCE_EXCLUDES = frozenset({".git", "WWMI_SOURCE_MANIFEST.json", "__pycache__"})
_STAGE_CACHES = frozenset({"__pycache__"})


def stage_all(repo_root: Path, upstream_root: Path, stage_root: Path) -> None:
    up_reshade = upstream_root / "reshade"
    up_wwmi = upstream_root / "wwmi-dx12"
    tdm_root = upstream_root / "3Dmigoto"
    up_3dm = tdm_root / "DirectX11"
    if not (up_reshade / "include/reshade.hpp").is_file():
        raise FileNotFoundError(
            f"Missing upstream ReShade at {up_reshade} — run `python3 scripts/bootstrap_deps.py` first."
        )
    if not (up_wwmi / "reshade_addon").is_dir():
        raise FileNotFoundError(
            f"Missing upstream wwmi-dx12 at {up_wwmi} — run `python3 scripts/bootstrap_deps.py` first."
        )
    if not (up_3dm / "CommandList.cpp").is_file():
        raise FileNotFoundError(
            f"Missing upstream 3Dmigoto at {tdm_root} — run `python3 scripts/bootstrap_deps.py` first."
        )

    reshade = files_under(up_reshade, _SOURCE_EXCLUDES)
    reshade.update(files_under(repo_root / "source/host", _SOURCE_EXCLUDES))
    wwmi = files_under(up_wwmi, _SOURCE_EXCLUDES)
    wwmi.update(files_under(repo_root / "source/main", _SOURCE_EXCLUDES | {"3dm_override"}))

    ov_3dm = repo_root / "source/main/3dm_override"
    tdm_subset = (
        "CommandList.cpp",
        "CommandList.h",
        "D3D11Wrapper.h",
        "DLLMainHook.h",
        "DecompileHLSL.h",
        "DrawCallInfo.h",
        "FrameAnalysis.h",
        "HackerDXGI.h",
        "HackerInputLayout.cpp",
        "HackerInputLayout.h",
        "HookedContext.h",
        "HookedDXGI.h",
        "HookedDevice.h",
        "Hunting.h",
        "IniHandler.cpp",
        "IniHandler.h",
        "Overlay.h",
        "Override.cpp",
        "Override.h",
        "PointerSet.h",
        "ResourceHash.cpp",
        "ResourceHash.h",
        "ShaderRegex.h",
        "cursor.cpp",
        "cursor.h",
        "globals.h",
        "input.cpp",
        "input.h",
        "lock.cpp",
        "lock.h",
        "log.h",
        "nvprofile.h",
        "profiling.cpp",
        "profiling.h",
        "shader.h",
        "util.cpp",
        "util.h",
        "util_min.h",
        "version.h",
        "vkeys.h",
    )
    engine = files_under(ov_3dm, _SOURCE_EXCLUDES)
    for fname in tdm_subset:
        rel = Path(fname)
        if rel not in engine:
            for candidate in (up_3dm / fname, tdm_root / fname):
                if candidate.is_file():
                    engine[rel] = candidate
                    break
            else:
                raise FileNotFoundError(f"Missing pinned 3Dmigoto source: {fname}")

    trees = {"reshade": reshade, "wwmi-dx12": wwmi, "3dm_src": engine}
    archive = Archive("wwmi-stale-sources")
    expected = {Path(name) / rel for name, files in trees.items() for rel in files}
    prune_tree(stage_root, expected, archive, Path("_staged"), _STAGE_CACHES)
    count = sum(sync_file(src, stage_root / name / rel)
                for name, files in trees.items() for rel, src in files.items())
    print(f"[stage] {count} updated, {len(archive.entries)} archived -> {stage_root}")
    if archive.root:
        print(f"[stage] archive: {archive.root}")


def main() -> int:
    ap = argparse.ArgumentParser(description="Stage upstream + overlay sources for CMake build.")
    ap.add_argument("--repo-root", type=Path, required=True)
    ap.add_argument("--upstream-root", type=Path, required=True)
    ap.add_argument("--stage-root", type=Path, required=True)
    args = ap.parse_args()
    stage_all(args.repo_root.resolve(), args.upstream_root.resolve(), args.stage_root.resolve())
    return 0


if __name__ == "__main__":
    sys.exit(main())
