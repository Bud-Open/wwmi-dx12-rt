#!/usr/bin/env python3
"""Assemble portable release packages for both Windows (Official Kuro Launcher / Steam)
and Linux / SteamOS (Steam Proton / Steam Deck).

Outputs into gitignored dist/:
  - dist/WWMI-DX12-RT-win64/      + dist/WWMI-DX12-RT-win64.zip
  - dist/WWMI-DX12-RT-linux/      + dist/WWMI-DX12-RT-linux.tar.gz
"""
from __future__ import annotations

import argparse
import datetime as _dt
import json
import subprocess
import shutil
import sys
import tarfile
import zipfile
from pathlib import Path

from bootstrap_deps import (
    DXC_COMMIT, DXC_RELEASE_URL, DXC_URL, PINNED_GIT, git_output,
)

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
import installer_core as core  # noqa: E402


WINDOWS_GUIDE = """\
========================================================================
  WWMI-DX12-RT (Windows Portable — Official Kuro Launcher & Steam)
========================================================================

[Prerequisites]
- Run Wuthering Waves in DirectX 12 mode (do NOT enable "Launch with DX11").
- Keep in-game Character Detail / LOD Bias on High.

[Quick Start — One-Time Setup]
1. Extract this ZIP to a fixed folder OUTSIDE the game directory
   (e.g. D:\\Tools\\WWMI-DX12-RT — never extract into Client\\Binaries\\Win64).
2. Run `WWMI-DX12-RT-Setup.exe`:
   - Auto-detects Wuthering Waves (Kuro Launcher & Steam); click Browse for custom paths.
   - Auto-links an existing `%AppData%\\XXMI Launcher\\WWMI` folder.
     If you do not have XXMI, click [Init Skeleton], place the official WWMI-Package
     `Core\\` folder inside it, and keep at most ONE copy of `RabbitFX` under `Mods\\`.
   - Click [Install & Enable], then close the setup tool once it succeeds.
3. Play normally:
   - Launch Wuthering Waves directly from the official Kuro Launcher or Steam.
   - In-game hotkeys: [F10] Hot-Reload mods, [F6] Toggle mods on/off, [Home] Overlay & status.
4. Upgrade & Uninstall:
   - To upgrade, extract over your existing external folder (preserving
     `runtime\\install-ledger.json`) and re-run [Install & Enable], OR click
     [Uninstall / Restore] in the old folder before moving to a new directory.
   - Click [Uninstall / Restore] at any time to cleanly remove the 2 bootstrap files from Win64.
5. Coexisting with Existing ReShade / Third-Party Add-ons (e.g. RenoDX):
   - WWMI-DX12-RT requires its paired `dxgi.dll` (do NOT replace it with stock ReShade `dxgi.dll`).
   - If you already had ReShade in Win64, back up/move the old `dxgi.dll` and `ReShade.ini` first,
     then run [Install & Enable].
   - To load other ReShade add-ons (e.g. RenoDX), either copy their `*.addon64` into
     `<ExternalFolder>\\reshade-addons\\`, OR add `LoadGameDirAddons=1` under `[ADDON]` in
     `<ExternalFolder>\\ReShade.ini` (your external `ReShade.ini` customizations are preserved on upgrades).
   - If mods stop working in-game, temporarily disable third-party add-ons to rule out hook conflicts.

========================================================================
  WWMI-DX12-RT (Windows 便携版 — 支持库洛官方启动器 / Steam)
========================================================================

【使用前提】
- 必须使用 DirectX 12 模式启动游戏（切勿勾选「使用 DX11 启动」）。
- 游戏内「角色精细度 / LOD」建议保持为「高」。

【快速使用（仅需首次操作一次，后续按原习惯直启）】
1. 将本压缩包完整解压到游戏目录之外的任意固定目录（例如 D:\\Tools\\WWMI-DX12-RT，切勿解压进游戏 Win64 目录）。
2. 双击运行 `WWMI-DX12-RT-Setup.exe`：
   - 自动探测库洛官方启动器与 Steam 版《鸣潮》目录（自定义路径点「浏览」选择即可）；
   - 自动关联现有 `%AppData%\\XXMI Launcher\\WWMI` 目录；若未装过 XXMI，可点【新建骨架】，把官方 `WWMI-Package` 的 `Core\\` 放入其中（需要 RabbitFX 的皮肤在 `Mods\\` 下仅保留一份）；
   - 点击【一键安装启用】，提示成功后关闭即可。
3. 日常游玩：
   - 直接打开库洛官方启动器或 Steam 点击「开始游戏」即可；
   - 快捷键：`F10` 热重载、`F6` 实时开关 Mod、`Home` 打开状态面板。
4. 升级与彻底卸载：
   - 升级新版时，直接解压覆盖原外部目录（保留 `runtime\\install-ledger.json`）后重跑【一键安装启用】，或先在旧目录点击【一键卸载还原】再迁移新目录。
   - 随时点击【一键卸载还原】，即可安全清除 Win64 中的 2 个引导文件恢复纯净原厂目录。
5. 与现有 ReShade / 第三方插件（如 RenoDX）共存：
   - WWMI-DX12-RT 必须使用本包配对的 `dxgi.dll`（切勿用官方原版 ReShade `dxgi.dll` 替换）。
   - 若游戏 Win64 原先已装 ReShade，先备份移走原 `dxgi.dll` 与 `ReShade.ini` 后再安装。
   - 第三方 `*.addon64`（如 RenoDX）可放入外部 `reshade-addons\\`，或在外部 `ReShade.ini` 的 `[ADDON]` 下添加 `LoadGameDirAddons=1`（升级重装会保留你的外部 `ReShade.ini` 配置）。
   - 若游戏内 Mod 异常，请先临时停用第三方插件排查挂钩冲突。
"""

LINUX_GUIDE = """\
========================================================================
  WWMI-DX12-RT (Linux / SteamOS / Steam Deck Portable — Proton)
========================================================================

[Prerequisites]
- Run Wuthering Waves in DirectX 12 mode (do NOT pass -dx11).
- If you initialize a new WWMI skeleton instead of linking an existing XXMI folder,
  place the official WWMI-Package `Core/` folder inside it (and keep only ONE copy
  of `RabbitFX` under `Mods/` if required by skins).

[Option 1: GUI Wizard (Desktop & Steam Deck Desktop Mode)]
1. Extract this archive to a fixed folder (e.g. ~/Tools/WWMI-DX12-RT) and run:
     ./wwmi-dx12-setup gui
   (Optional: `./wwmi-dx12-setup desktop-entry` creates an application menu shortcut.)
2. Choose Install & Enable — auto-detects Native & Flatpak Steam libraries and places
   only `dxgi.dll` + `ReShade.ini` into Client/Binaries/Win64 (zero runtime writes).
3. One-time Steam Launch Options (Steam -> Properties -> Launch Options):
   - ASCII mod folder names:
       WINEDLLOVERRIDES="dxgi=n,b" %command%
   - Non-ASCII / Chinese mod folder names:
       Run `./wwmi-dx12-setup install --proton-utf8-guard` (or pass `--proton-utf8-guard`
       to `gui`) and paste the printed launch options (`HOST_LC_ALL=C.UTF-8`, plus
       optional preload shim if extract/install paths contain no spaces or colons).
4. Click Play in Steam directly. Hotkeys: [F10] Hot-Reload, [F6] Toggle, [Home] Overlay.

[Option 2: CLI Commands]
- Health check (incl. Flatpak sandbox & RabbitFX): ./wwmi-dx12-setup doctor
- Install & enable:                                ./wwmi-dx12-setup install --init-missing-wwmi
- Safe uninstall & restore:                        ./wwmi-dx12-setup uninstall

[Upgrading]
- Re-run install using the same external folder (preserving `runtime/install-ledger.json`),
  or run `./wwmi-dx12-setup uninstall` before switching to a new folder.

========================================================================
  WWMI-DX12-RT (Linux / SteamOS / Steam Deck 通用便携版)
========================================================================

【使用前提】
- 必须使用 DirectX 12 模式启动游戏（切勿启用 `-dx11`）。
- 若选择自动新建骨架而非关联现有 XXMI 目录，请将官方 `WWMI-Package` 的 `Core/` 放入该 WWMI 目录（需要 RabbitFX 的皮肤在 `Mods/` 下仅保留一份）。

【方式一：图形向导安装（推荐桌面环境与 Steam Deck 桌面模式）】
1. 解压本压缩包到固定目录后执行 `./wwmi-dx12-setup gui`（可选 `./wwmi-dx12-setup desktop-entry` 生成菜单图标）。
2. 选择「一键安装启用」：自动识别原生 Steam / Flatpak Steam 跨盘库，仅向游戏 Win64 写入 `dxgi.dll` 与 `ReShade.ini` 两个引导文件。
3. 首次配置 Steam 启动选项：
   - 纯英文 Mod 目录名：`WINEDLLOVERRIDES="dxgi=n,b" %command%`
   - 含中文文件夹名 Mod：运行 `./wwmi-dx12-setup install --proton-utf8-guard`（图形向导支持同名参数），复制输出填入 **Steam → 属性 → 启动选项**；使用 preload shim 时解压和安装路径不要包含空格或冒号。
4. 之后直接在 Steam 点击「开始游戏」即可；游戏内支持 `F10` 热重载、`F6` 开关 Mod、`Home` 打开状态面板。

【方式二：命令行一键操作】
- 环境与 Flatpak/RabbitFX 自检：`./wwmi-dx12-setup doctor`
- 一键安装启用：                `./wwmi-dx12-setup install --init-missing-wwmi`
- 一键安全卸载还原：            `./wwmi-dx12-setup uninstall`

【升级新版】
- 在原外部挂载目录覆盖后重跑安装（保留 `runtime/install-ledger.json`），或先执行卸载后再更换目录。
"""


def verify_build_manifest(stage5: Path, include_linux: bool) -> dict | None:
    manifest_path = stage5 / "build-manifest.json"
    if not manifest_path.is_file():
        return None
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    for rel, expected_sha in manifest.get("stage5_sha256", {}).items():
        target = stage5 / rel
        if not target.is_file() or core.sha256_file(target) != expected_sha:
            raise RuntimeError(
                f"Staged artifact mismatch for {rel} vs build-manifest.json — re-run `python3 scripts/build.py`."
            )
    if include_linux:
        linux_so = ROOT / "lib/wine-filename-utf8.so"
        expected_so = manifest.get("linux_so_sha256")
        if expected_so and (not linux_so.is_file() or core.sha256_file(linux_so) != expected_so):
            raise RuntimeError(
                "lib/wine-filename-utf8.so does not match build-manifest.json — re-run `python3 scripts/build.py`."
            )
    return manifest


def build_provenance(manifest: dict | None) -> dict:
    commit = manifest.get("repository_commit") if manifest else None
    dirty = manifest.get("working_tree_dirty") if manifest else None
    if commit is None and (ROOT / ".git").exists():
        try:
            commit = git_output(ROOT, "rev-parse", "HEAD")
            dirty = bool(git_output(ROOT, "status", "--porcelain"))
        except (OSError, subprocess.CalledProcessError):
            pass
    return {
        "repository_commit": commit,
        "working_tree_dirty": dirty,
        "upstreams": [
            *({"name": name, "url": url, "commit": sha} for name, url, sha in PINNED_GIT),
            {"name": "DirectXShaderCompiler", "url": DXC_URL, "commit": DXC_COMMIT,
             "release_asset": DXC_RELEASE_URL},
        ],
        "entry_points": [
            "python3 scripts/bootstrap_deps.py",
            "python3 scripts/build.py",
            "python3 scripts/package_release.py",
        ],
    }


def populate_common_runtime(dst_root: Path, payload_map: dict[str, Path], include_linux_so: bool) -> None:
    for rel, src in payload_map.items():
        if rel.startswith("lib/") and not include_linux_so:
            continue
        out = dst_root / rel
        out.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, out)

    (dst_root / "seed").mkdir(parents=True, exist_ok=True)
    # Do NOT bundle a live ReShade.ini in release archives: extracting an upgrade over an
    # existing external folder would overwrite user customizations (LoadGameDirAddons, presets,
    # ExtraAddonPaths) before Setup runs. Both installers create ReShade.ini on first install.

    for root_doc in ("LICENSE", "THIRD-PARTY-NOTICES.md"):
        doc_src = ROOT / root_doc
        if doc_src.is_file():
            shutil.copy2(doc_src, dst_root / root_doc)

    lic_src = ROOT / "payload/licenses"
    if lic_src.is_dir():
        for p in lic_src.rglob("*"):
            if p.is_file():
                rel = p.relative_to(lic_src)
                target = dst_root / "licenses" / rel
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(p, target)


def make_zip(src_dir: Path, out_zip: Path) -> None:
    with zipfile.ZipFile(
        out_zip, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6, strict_timestamps=False
    ) as zf:
        for p in sorted(src_dir.rglob("*")):
            if p.is_file():
                arcname = f"{src_dir.name}/{p.relative_to(src_dir).as_posix()}"
                zf.write(p, arcname)


LINUX_EXECUTABLES = {"wwmi-dx12-setup", "scripts/wwmi-dx12-setup.py",
                     "tools/texlook_tool.py", "tools/migrate_hash_free.py"}


def make_targz(src_dir: Path, out_tar: Path) -> None:
    def linux_mode(info: tarfile.TarInfo) -> tarfile.TarInfo:
        relative = info.name.partition("/")[2]
        info.mode = 0o755 if info.isdir() or relative in LINUX_EXECUTABLES else 0o644
        info.uid = 0
        info.gid = 0
        info.uname = "root"
        info.gname = "root"
        return info

    with tarfile.open(out_tar, "w:gz") as tf:
        tf.add(src_dir, arcname=src_dir.name, filter=linux_mode)


def main() -> int:
    ap = argparse.ArgumentParser(description="Assemble portable Windows and/or Linux release packages.")
    ap.add_argument(
        "--target",
        choices=("auto", "all", "win64", "linux"),
        default="auto",
        help="Package target (default: 'all' on Linux hosts, 'win64' on Windows hosts)",
    )
    args = ap.parse_args()
    target = ("all" if sys.platform.startswith("linux") else "win64") if args.target == "auto" else args.target
    build_win = target in ("all", "win64")
    build_linux = target in ("all", "linux")

    payload_map = core.resolve_payload_sources(ROOT)
    if build_linux and "lib/wine-filename-utf8.so" not in payload_map:
        raise FileNotFoundError(
            "Missing lib/wine-filename-utf8.so — build on Linux (`python3 scripts/build.py`) before packaging Linux tar.gz."
        )
    setup_exe = ROOT / "payload/artifacts/stage5/WWMI-DX12-RT-Setup.exe"
    if build_win:
        if not setup_exe.is_file():
            setup_exe = ROOT / "bin/WWMI-DX12-RT-Setup.exe"
        if not setup_exe.is_file():
            raise FileNotFoundError("Missing WWMI-DX12-RT-Setup.exe — run `python3 scripts/build.py` first.")

    manifest = verify_build_manifest(ROOT / "payload/artifacts/stage5", include_linux=build_linux)
    stamp = _dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    dist_base = ROOT / "dist" / f"release-{stamp}"
    provenance = json.dumps(build_provenance(manifest), ensure_ascii=False, indent=2) + "\n"

    print("Release packages built successfully:")
    if build_win:
        win_dir = dist_base / "WWMI-DX12-RT-win64"
        win_dir.mkdir(parents=True, exist_ok=True)
        (win_dir / "BUILD-PROVENANCE.json").write_text(provenance, encoding="utf-8")
        populate_common_runtime(win_dir, payload_map, include_linux_so=False)
        shutil.copy2(setup_exe, win_dir / "WWMI-DX12-RT-Setup.exe")
        (win_dir / "README-WINDOWS.txt").write_text(WINDOWS_GUIDE, encoding="utf-8", newline="\r\n")
        win_zip = dist_base / "WWMI-DX12-RT-win64.zip"
        make_zip(win_dir, win_zip)
        print(f"  [Windows] {win_zip} ({win_zip.stat().st_size:,} bytes)")

    if build_linux:
        lin_dir = dist_base / "WWMI-DX12-RT-linux"
        lin_dir.mkdir(parents=True, exist_ok=True)
        (lin_dir / "BUILD-PROVENANCE.json").write_text(provenance, encoding="utf-8")
        populate_common_runtime(lin_dir, payload_map, include_linux_so=True)
        for rel_tool in (
            "wwmi-dx12-setup",
            "scripts/wwmi-dx12-setup.py",
            "tools/installer_core.py",
            "tools/texlook_tool.py",
            "tools/migrate_hash_free.py",
            "tools/third_party/bcdec.h",
        ):
            src_t = ROOT / rel_tool
            dst_t = lin_dir / rel_tool
            dst_t.parent.mkdir(parents=True, exist_ok=True)
            if src_t.suffix == ".py" or rel_tool == "wwmi-dx12-setup":
                dst_t.write_text(src_t.read_text(encoding="utf-8"), encoding="utf-8", newline="\n")
            else:
                shutil.copy2(src_t, dst_t)
            if rel_tool in LINUX_EXECUTABLES:
                dst_t.chmod(0o755)
        (lin_dir / "README-LINUX.txt").write_text(LINUX_GUIDE, encoding="utf-8")
        lin_tar = dist_base / "WWMI-DX12-RT-linux.tar.gz"
        make_targz(lin_dir, lin_tar)
        print(f"  [Linux]   {lin_tar} ({lin_tar.stat().st_size:,} bytes)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
