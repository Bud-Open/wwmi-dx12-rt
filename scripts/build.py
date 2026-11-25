#!/usr/bin/env python3
"""Standalone CMake + Ninja build orchestrator for WWMI-DX12-RT.

Builds 100% of all binaries from source code + gitignored `third_party/`
(populated via `python3 scripts/bootstrap_deps.py`), with ZERO dependency on
legacy build plans, precompiled `.obj` files, or Clang VFS overlays:
  1. ReShade64-game-forward.dll  (paired D3D12/DXGI host)
  2. wwmi_dx12.addon64           (provider add-on + migoto_engine)
  3. zz_wwmi_rt_pack.addon64     (DXR Ray Tracing + Hybrid Raster consumer add-on)
  4. WWMI-DX12-RT-Setup.exe      (zero-dependency Win32 High-DPI GUI/CLI installer)
  5. lib/wine-filename-utf8.so   (Linux Wine UTF-8 filename locale shim, when built on Linux)

Assembles complete runtime outputs into `bin/` and `payload/artifacts/stage5/`.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

from build_support import Archive, prune_tree, sync_file
from runtime.wwmi_rt_extra_data import RT_PACK_SHADERS

ROOT = Path(__file__).resolve().parent.parent


def find_executable(names: tuple[str, ...]) -> str:
    for n in names:
        hit = shutil.which(n)
        if hit:
            return hit
    raise FileNotFoundError(f"Required build tool not found in PATH: {' / '.join(names)}")


def run_cmd(argv: list[str], cwd: Path) -> None:
    print(f"[cmd] {' '.join(str(x) for x in argv)}", flush=True)
    subprocess.run([str(x) for x in argv], cwd=cwd, check=True)


def locate_built_artifact(build_dir: Path, candidates: tuple[str, ...]) -> Path:
    for rel in candidates:
        p = build_dir / rel
        if p.is_file():
            return p
    raise FileNotFoundError(f"Expected build output not found under {build_dir}: {candidates}")


def maybe_compile_hlsl_shaders(dxc_exe: Path, out_dir: Path) -> Path:
    """Compile on Windows; Linux cross-builds use the corresponding checked-in CSOs."""
    src_dir = ROOT / "source/shaders/rt_pack"
    sources = [src_dir / Path(name).with_suffix(".hlsl") for name in RT_PACK_SHADERS]
    for source in sources:
        if not source.is_file():
            raise FileNotFoundError(f"Missing RT Pack HLSL source: {source}")

    if sys.platform != "win32":
        return src_dir / "assets"
    if not dxc_exe.is_file():
        raise FileNotFoundError(f"Missing {dxc_exe} — run `python3 scripts/bootstrap_deps.py`.")
    out_dir.mkdir(parents=True, exist_ok=True)
    for source, output in zip(sources, RT_PACK_SHADERS):
        run_cmd([str(dxc_exe), "-nologo", "-O3", "-T", "cs_6_0", "-E", "main",
                 "-Fo", str(out_dir / output), str(source)], ROOT)
    return out_dir


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while chunk := f.read(1 << 20):
            h.update(chunk)
    return h.hexdigest()


def write_build_manifest(stage5: Path, files: dict[Path, Path], linux_so: Path | None) -> Path:
    commit = dirty = None
    if (ROOT / ".git").exists():
        try:
            commit = subprocess.check_output(["git", "-C", str(ROOT), "rev-parse", "HEAD"], text=True).strip()
            dirty = bool(subprocess.check_output(["git", "-C", str(ROOT), "status", "--porcelain"], text=True).strip())
        except (OSError, subprocess.CalledProcessError):
            pass
    manifest = {
        "schema_version": 1,
        "repository_commit": commit,
        "working_tree_dirty": dirty,
        "host_platform": sys.platform,
        "stage5_sha256": {rel.as_posix(): sha256_file(stage5 / rel) for rel in sorted(files)},
    }
    if linux_so and linux_so.is_file():
        manifest["linux_so_sha256"] = sha256_file(linux_so)
    out = stage5 / "build-manifest.json"
    out.write_text(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    return out


def assemble_stage5(stage5: Path, files: dict[Path, Path]) -> None:
    for source in files.values():
        if not source.is_file() or not source.stat().st_size:
            raise FileNotFoundError(f"Missing or empty stage5 input: {source}")
    archive = Archive("wwmi-stale-stage5")
    prune_tree(stage5, set(files) | {Path("build-manifest.json")}, archive, Path("stage5"))
    for rel, source in files.items():
        sync_file(source, stage5 / rel)
    if archive.root:
        print(f"[stage5] archive: {archive.root}")


def main() -> int:
    ap = argparse.ArgumentParser(description="Build WWMI-DX12-RT from clean source via CMake & Ninja.")
    ap.add_argument(
        "--upstream-root",
        type=Path,
        default=ROOT / "third_party",
        help="Path to upstream dependencies (default: ./third_party)",
    )
    ap.add_argument(
        "--build-dir",
        type=Path,
        default=ROOT / "build",
        help="CMake build root directory (default: ./build)",
    )
    ap.add_argument(
        "--jobs",
        type=int,
        default=min(os.cpu_count() or 4, 16),
        help="Parallel build jobs",
    )
    args = ap.parse_args()

    upstream = args.upstream_root.resolve()
    if not (upstream / "reshade/include/reshade.hpp").is_file() or not (upstream / "wwmi-dx12/reshade_addon").is_dir():
        print(
            f"ERROR: Missing upstream dependencies under {upstream}.\n"
            f"Run `python3 scripts/bootstrap_deps.py` first to populate third_party/.",
            file=sys.stderr,
        )
        return 1

    cmake_bin = find_executable(("cmake",))
    ninja_bin = find_executable(("ninja", "ninja-build"))

    win_build = (args.build_dir / "win64").resolve()
    win_build.mkdir(parents=True, exist_ok=True)

    # 1. Configure & build Windows x64 runtime + Setup targets
    cfg_cmd = [
        cmake_bin,
        "-S",
        str(ROOT),
        "-B",
        str(win_build),
        "-G",
        "Ninja",
        f"-DCMAKE_MAKE_PROGRAM={ninja_bin}",
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DPython3_EXECUTABLE={sys.executable}",
        f"-DWWMI_UPSTREAM_ROOT={upstream}",
    ]
    if sys.platform != "win32":
        toolchain = ROOT / "cmake/toolchains/linux-clang-cl-xwin.cmake"
        cfg_cmd.append(f"-DCMAKE_TOOLCHAIN_FILE={toolchain}")

    run_cmd(cfg_cmd, ROOT)
    run_cmd(
        [
            cmake_bin,
            "--build",
            str(win_build),
            "--config",
            "Release",
            "--target",
            "ReShade",
            "wwmi_dx12",
            "zz_wwmi_rt_pack",
            "WWMI-DX12-RT-Setup",
            "-j",
            str(args.jobs),
        ],
        ROOT,
    )

    # 2. On Linux hosts, also build the native Linux Wine UTF-8 preload shim (.so)
    linux_so_built: Path | None = None
    if sys.platform.startswith("linux"):
        lin_build = (args.build_dir / "linux").resolve()
        lin_build.mkdir(parents=True, exist_ok=True)
        run_cmd(
            [
                cmake_bin,
                "-S",
                str(ROOT),
                "-B",
                str(lin_build),
                "-G",
                "Ninja",
                f"-DCMAKE_MAKE_PROGRAM={ninja_bin}",
                "-DCMAKE_BUILD_TYPE=Release",
            ],
            ROOT,
        )
        run_cmd(
            [
                cmake_bin,
                "--build",
                str(lin_build),
                "--target",
                "wine-filename-utf8",
                "-j",
                str(args.jobs),
            ],
            ROOT,
        )
        linux_so_built = locate_built_artifact(lin_build, ("wine-filename-utf8.so",))
        lib_dir = ROOT / "lib"
        lib_dir.mkdir(parents=True, exist_ok=True)
        shutil.copy2(linux_so_built, lib_dir / "wine-filename-utf8.so")

    # 3. Collect outputs into bin/ and payload/artifacts/stage5/
    host_dll = locate_built_artifact(
        win_build,
        ("reshade_host/ReShade64.dll", "reshade_host/Release/ReShade64.dll"),
    )
    main_addon = locate_built_artifact(
        win_build,
        ("wwmi_dx12.addon64", "Release/wwmi_dx12.addon64"),
    )
    client_addon = locate_built_artifact(
        win_build,
        ("zz_wwmi_rt_pack.addon64", "Release/zz_wwmi_rt_pack.addon64"),
    )
    setup_exe = locate_built_artifact(
        win_build,
        ("WWMI-DX12-RT-Setup.exe", "Release/WWMI-DX12-RT-Setup.exe"),
    )

    bin_dir = ROOT / "bin"
    bin_dir.mkdir(parents=True, exist_ok=True)
    shutil.copy2(host_dll, bin_dir / "ReShade64.dll")
    shutil.copy2(main_addon, bin_dir / "wwmi_dx12.addon64")
    shutil.copy2(client_addon, bin_dir / "zz_wwmi_rt_pack.addon64")
    shutil.copy2(setup_exe, bin_dir / "WWMI-DX12-RT-Setup.exe")

    stage5 = ROOT / "payload/artifacts/stage5"
    files = {
        Path("ReShade64-game-forward.dll"): host_dll,
        Path("wwmi_dx12.addon64"): main_addon,
        Path("zz_wwmi_rt_pack.addon64"): client_addon,
        Path("WWMI-DX12-RT-Setup.exe"): setup_exe,
    }
    cso_src = ROOT / "source/main/shaders_cso"
    if not (cso_src / "Bc7Half.cso").is_file():
        cso_src = upstream / "wwmi-dx12/shaders_cso"
    for name in ("Bc7Half.cso", "BufCopy.cso", "IbVbHash.cso"):
        files[Path("wwmi_dx12") / name] = cso_src / name

    dxc_dir = upstream / "dxc"
    for name in ("dxcompiler.dll", "dxil.dll"):
        files[Path("wwmi_dx12/dxc") / name] = dxc_dir / name

    assets = maybe_compile_hlsl_shaders(dxc_dir / "dxc.exe", win_build / "rt-pack-shaders")
    for name in RT_PACK_SHADERS:
        files[Path("rt-pack-data") / name] = assets / name
    assemble_stage5(stage5, files)
    write_build_manifest(stage5, files, (ROOT / "lib/wine-filename-utf8.so") if linux_so_built else None)

    print("\nPACKAGED STANDALONE BUILD OUTPUTS:")
    for f in sorted(stage5.iterdir()):
        if f.is_file():
            print(f"  {f.name:30s}: {f.stat().st_size:,} bytes")
    if linux_so_built and (ROOT / "lib/wine-filename-utf8.so").is_file():
        sz = (ROOT / "lib/wine-filename-utf8.so").stat().st_size
        print(f"  {'lib/wine-filename-utf8.so':30s}: {sz:,} bytes")
    return 0


if __name__ == "__main__":
    sys.exit(main())
