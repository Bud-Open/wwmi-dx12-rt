#!/usr/bin/env python3
"""Bootstrap pinned open-source upstream dependencies into gitignored third_party/
for standalone clean-source CMake builds (Windows MSVC / ClangCL or Linux cross-compile)
with ZERO dependency on legacy local build trees.

Pinned baselines:
  - crosire/reshade          @ 05c95f4ac083d1c4c1ab846ffe65bf4b775d3bf9 (BSD-3-Clause)
  - lekarsenten/wwmi-dx12    @ ec837ef8ab798b7c251105a7448efeceb526a2c9 (GPL-3.0)
    + SpectrumQT/3Dmigoto    @ a7847eae (submodule, GPL-3.0)
  - DirectXShaderCompiler    @ v1.8.2502 (dxc_2025_02_20.zip -> dxcompiler.dll, dxil.dll, dxc.exe)
"""
from __future__ import annotations

import argparse
import io
import shutil
import subprocess
import sys
import urllib.request
import zipfile
from pathlib import Path

from build_support import Archive

ROOT = Path(__file__).resolve().parent.parent
THIRD_PARTY = ROOT / "third_party"

RESHADE_URL = "https://github.com/crosire/reshade.git"
RESHADE_COMMIT = "05c95f4ac083d1c4c1ab846ffe65bf4b775d3bf9"

WWMI_DX12_URL = "https://github.com/lekarsenten/wwmi-dx12.git"
WWMI_DX12_COMMIT = "ec837ef8ab798b7c251105a7448efeceb526a2c9"

MIGOTO_URL = "https://github.com/SpectrumQT/3Dmigoto.git"
MIGOTO_COMMIT = "a7847eae82aaac32175c9a7d199673d5c9323bcd"
DXC_URL = "https://github.com/microsoft/DirectXShaderCompiler.git"
DXC_COMMIT = "b4711839eb9a87da7c3436d9b212e0492359fbbd"
DXC_RELEASE_URL = (
    "https://github.com/microsoft/DirectXShaderCompiler/releases/download/"
    "v1.8.2502/dxc_2025_02_20.zip"
)
DXC_FILES = ("dxcompiler.dll", "dxil.dll", "dxc.exe")

PINNED_GIT = (
    ("reshade", RESHADE_URL, RESHADE_COMMIT),
    ("wwmi-dx12", WWMI_DX12_URL, WWMI_DX12_COMMIT),
)


def git_output(root: Path, *args: str) -> str:
    return subprocess.check_output(
        ["git", "-C", str(root), *args], text=True, stderr=subprocess.PIPE
    ).rstrip("\r\n")


def checkout_issue(root: Path, commit: str) -> str | None:
    if not root.is_dir() or not (root / ".git").exists():
        return "missing Git checkout"
    try:
        if git_output(root, "rev-parse", "HEAD") != commit:
            return f"HEAD does not match {commit}"
        if git_output(root, "ls-files", "--deleted"):
            return "tracked files missing"
        for line in git_output(root, "submodule", "status", "--recursive").splitlines():
            if not line.startswith(" "):
                return f"submodule not at its pinned commit: {line}"
        # An initialized gitdir alone is insufficient if the worktree was
        # partially copied or emptied. Check the tracked files in each child.
        if git_output(root, "submodule", "foreach", "--quiet", "--recursive", "git ls-files --deleted"):
            return "submodule tracked files missing"
    except (OSError, subprocess.CalledProcessError) as exc:
        return f"cannot validate Git checkout: {exc}"
    return None


def archive_incomplete(dst: Path, issue: str, archive: Archive) -> None:
    if dst.exists() or dst.is_symlink():
        print(f"[repair] {dst.name}: {issue}", flush=True)
        archive.move(dst, Path(dst.name))
        print(f"[repair] saved to {archive.root}", flush=True)


def fetch_pinned_git(name: str, url: str, commit: str, dry_run: bool, archive: Archive,
                     seed_dir: Path | None = None) -> None:
    dst = THIRD_PARTY / name
    issue = checkout_issue(dst, commit)
    if issue is None:
        print(f"[ok] {name} @ {commit} with recursive submodules")
        return
    if seed_dir is not None:
        seed = seed_dir.resolve()
        if error := checkout_issue(seed, commit):
            raise ValueError(f"Invalid {name} seed at {seed}: {error}")
        print(f"[seed] {name} from {seed} -> {dst} ({issue})", flush=True)
    else:
        print(f"[fetch] {name} ({url} @ {commit}) -> {dst} ({issue})", flush=True)
    if dry_run:
        return

    archive_incomplete(dst, issue, archive)
    if seed_dir is not None:
        shutil.copytree(seed, dst, symlinks=True)
    else:
        dst.mkdir(parents=True, exist_ok=True)
        subprocess.run(["git", "init", "-q", str(dst)], check=True)
        subprocess.run(["git", "-C", str(dst), "remote", "add", "origin", url], check=True)
        subprocess.run(["git", "-C", str(dst), "fetch", "--depth", "1", "origin", commit], check=True)
        subprocess.run(["git", "-C", str(dst), "checkout", "-q", "FETCH_HEAD"], check=True)
        subprocess.run(
            ["git", "-C", str(dst), "submodule", "update", "--init", "--recursive", "--depth", "1"],
            check=True,
        )
    if error := checkout_issue(dst, commit):
        raise RuntimeError(f"Incomplete {name} checkout: {error}")


def missing_dxc(root: Path, names: tuple[str, ...]) -> list[str]:
    return [name for name in names if not (root / name).is_file() or not (root / name).stat().st_size]


def fetch_dxc(dry_run: bool, archive: Archive, seed_dir: Path | None = None) -> None:
    dst = THIRD_PARTY / "dxc"
    required = DXC_FILES if sys.platform == "win32" else DXC_FILES[:2]
    missing = missing_dxc(dst, required)
    if not missing:
        print(f"[ok] dxc: {', '.join(required)}")
        return

    if seed_dir is not None:
        seed = seed_dir.resolve()
        bin_src = seed / "bin/x64" if (seed / "bin/x64").is_dir() else seed
        if absent := missing_dxc(bin_src, required):
            raise FileNotFoundError(f"Missing or empty DXC seed files in {bin_src}: {', '.join(absent)}")
        print(f"[seed] DXC from {bin_src} -> {dst}", flush=True)
    else:
        print(f"[fetch] DirectXShaderCompiler v1.8.2502 ({DXC_RELEASE_URL}) -> {dst}", flush=True)
    if dry_run:
        return

    archive_incomplete(dst, f"missing or empty: {', '.join(missing)}", archive)
    dst.mkdir(parents=True)
    if seed_dir is not None:
        for name in DXC_FILES:
            if name not in missing_dxc(bin_src, (name,)):
                shutil.copyfile(bin_src / name, dst / name)
    else:
        with urllib.request.urlopen(DXC_RELEASE_URL, timeout=60) as response:
            data = io.BytesIO(response.read())
        with zipfile.ZipFile(data) as zf:
            for name in DXC_FILES:
                with zf.open(f"bin/x64/{name}") as src_f, (dst / name).open("wb") as dst_f:
                    shutil.copyfileobj(src_f, dst_f)
    if absent := missing_dxc(dst, required):
        raise RuntimeError(f"Incomplete DXC runtime: {', '.join(absent)}")


def main() -> int:
    ap = argparse.ArgumentParser(
        description="Fetch pinned upstream dependencies into third_party/ for standalone CMake builds."
    )
    ap.add_argument("--dry-run", action="store_true", help="Print actions without performing network/disk writes")
    ap.add_argument("--seed-reshade", type=Path, help="Optional local ReShade baseline directory for offline builds")
    ap.add_argument("--seed-wwmi", type=Path, help="Optional local wwmi-dx12 baseline directory for offline builds")
    ap.add_argument("--seed-dxc", type=Path, help="Local DXC seed: dxcompiler.dll, dxil.dll, and dxc.exe on Windows")
    args = ap.parse_args()

    print(f"Target third_party root: {THIRD_PARTY}")
    archive = Archive("wwmi-incomplete-deps")
    for (name, url, commit), seed in zip(PINNED_GIT, (args.seed_reshade, args.seed_wwmi)):
        fetch_pinned_git(name, url, commit, args.dry_run, archive, seed)
    fetch_dxc(args.dry_run, archive, args.seed_dxc)

    print("\nBootstrap ready. Build all targets from source via:")
    print("  python3 scripts/build.py")
    return 0


if __name__ == "__main__":
    sys.exit(main())
