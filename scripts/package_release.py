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
from typing import Sequence

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
4. Side-by-Side Upgrade (incl. from v1.0.2 / v1.0.3), Rollback & Uninstall:
   - Recommended side-by-side upgrade: extract the new release ZIP into a NEW folder
     (e.g. D:\\Tools\\WWMI-DX12-RT-v1.0.4) and run `WWMI-DX12-RT-Setup.exe`.
     Setup verifies the existing Win64 install (including legacy v1.0.2/v1.0.3 folders) against
     the old folder's ledger, paired host DLL, and SHA-256 digests, automatically migrates your
     `ReShade.ini` customizations, `wwmi_dx12.ini` user settings & active `wwmi_dir`, compatible
     caches, and third-party `*.addon64` add-ons, retains your populated `WWMI` folder, and keeps
     the old folder intact.
   - Instant rollback (works even back to v1.0.2 / v1.0.3): click [Rollback] inside the NEW
     version's `WWMI-DX12-RT-Setup.exe` (or run `WWMI-DX12-RT-Setup.exe --cli rollback`) to
     restore the previous folder's bootstrap pair and ledger cleanly.
   - In-place refresh (for v1.0.2/v1.0.3 overwrite users): extracting over the same folder
     (keeping `runtime\\install-ledger.json`, `ReShade.ini`, `wwmi_dx12.ini`) and running Setup
     once refreshes `Win64\\dxgi.dll` and preserves your settings (does not keep old binaries for rollback).
   - Seamless In-App Updates: `WWMI-DX12-RT-Setup.exe` checks `update-manifest.json` on launch
     (can be toggled off under [More -> Auto-check updates on launch]; the in-game `dxgi.dll`
     always stays 100% offline). When a newer release is available, click [Update Now] to download,
     verify SHA-256 checksums, deploy into an isolated `versions\\<ver>\\` slot, and switch cleanly
     while keeping your settings, caches, Mods, third-party add-ons, and one-click rollback intact.
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
4. 并排升级（含 v1.0.2 / v1.0.3 老用户）、一键回滚与彻底卸载：
   - 推荐并排升级：将新版压缩包解压到**新的独立目录**（如 D:\\Tools\\WWMI-DX12-RT-v1.0.4），双击运行新版 `WWMI-DX12-RT-Setup.exe` 点击【并排升级】。工具会核验旧目录（含 v1.0.2/v1.0.3 老版）账本、配对宿主 DLL 与 Win64 双文件 SHA-256，自动迁移你的 `ReShade.ini` 自定义项、`wwmi_dx12.ini` 用户参数与生效 `wwmi_dir`、兼容缓存与第三方 `*.addon64`，默认沿用已有 `WWMI` Mod 目录并完整保留旧目录。
   - 一键回滚（支持回滚至 v1.0.2 / v1.0.3）：在新版 `WWMI-DX12-RT-Setup.exe` 中点击【回滚旧版】（或命令行执行 `WWMI-DX12-RT-Setup.exe --cli rollback`），即可自动恢复旧目录的引导桥与账本。
   - 原地覆盖刷新：v1.0.2 / v1.0.3 老用户也可直接解压覆盖原目录（保留 `runtime\\install-ledger.json` 与 `ReShade.ini`/`wwmi_dx12.ini`），覆盖后运行一次新版 `WWMI-DX12-RT-Setup.exe` 点【原地刷新】即可同步更新 `Win64\\dxgi.dll` 并写入新版本号（覆盖方式不保留旧载荷回滚）。
   - 应用内无感升级：打开 `WWMI-DX12-RT-Setup.exe` 时自动异步检查 `update-manifest.json`（可在【更多操作】关闭，游戏进程内 `dxgi.dll` 始终保持 100% 离线零联网）。发现新版本时点击【一键升级】，即可自动完成下载、SHA-256 校验、独立 `versions\\<版本>\\` 槽位部署与无缝切换，完整保留外部配置、缓存、Mod 与一键回滚能力。
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
- In-place install / refresh (default XDG base):   ./wwmi-dx12-setup install --init-missing-wwmi
- Side-by-side upgrade (explicit --base-root):     ./wwmi-dx12-setup install --base-root ~/Tools/WWMI-DX12-RT-v1.0.4
- Roll back to preserved previous folder:          ./wwmi-dx12-setup rollback --base-root ~/Tools/WWMI-DX12-RT-v1.0.4
- Safe uninstall & restore:                        ./wwmi-dx12-setup uninstall
- In-app verified update (keeps LD_PRELOAD fixed): ./wwmi-dx12-setup update
- Check for updates (GitHub Releases manifest):    ./wwmi-dx12-setup check-update

[Upgrading (incl. v1.0.2 / v1.0.3) & Rollback]
- Default In-Place Refresh: Linux setup defaults `--base-root` to the XDG/Flatpak runtime
  root (`~/.local/share/wwmi-dx12-rt` or Flatpak Steam data dir), independent of where you
  extracted the archive. Running `./wwmi-dx12-setup install` without `--base-root` refreshes
  that active root in place while preserving your `ReShade.ini`, `wwmi_dx12.ini`, caches, and
  `WWMI` directory (works seamlessly for v1.0.2 / v1.0.3 users!).
- Side-by-Side Upgrade with Instant Rollback: pass a distinct `--base-root` for the new
  version (e.g. `./wwmi-dx12-setup install --base-root ~/Tools/WWMI-DX12-RT-v1.0.4`). Setup
  verifies the active Win64 files against the previous root's ledger, paired host DLL, and
  SHA-256 digests, migrates your settings/caches/third-party `*.addon64`, retains your active
  `WWMI` Mod tree, and preserves the old root intact (pass `--migrate-from <old_root>` if
  Win64's BasePath cannot be reverse-mapped automatically).
- Roll back (works even back to v1.0.2 / v1.0.3): run `./wwmi-dx12-setup rollback --base-root <new_root>`
  from the NEW package to restore the previous root's bootstrap pair and ledger cleanly.
- If your base root changed and Steam Launch Options use `LD_PRELOAD`, update the
  `wine-filename-utf8.so` path in Steam manually (Setup never modifies Steam configs).
- Verify downloaded release archives against `SHA256SUMS.txt`.

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
- 默认原地安装 / 升级刷新：     `./wwmi-dx12-setup install --init-missing-wwmi`
- 指定目录并排升级（可回滚）：  `./wwmi-dx12-setup install --base-root ~/Tools/WWMI-DX12-RT-v1.0.4`
- 一键回滚至旧版目录：          `./wwmi-dx12-setup rollback --base-root ~/Tools/WWMI-DX12-RT-v1.0.4`
- 一键安全卸载还原：            `./wwmi-dx12-setup uninstall`
- 一键在线升级（路径不变）：    `./wwmi-dx12-setup update`
- 检查新版本（直连清单零限流）：`./wwmi-dx12-setup check-update`

【升级（含 v1.0.2 / v1.0.3 老用户）与一键回滚】
- 默认原地刷新：Linux 版默认挂载根固定在 XDG/Flatpak 目录（与解压目录无关），直接运行 `./wwmi-dx12-setup install` 会在原挂载根原地刷新载荷，自动保留 `ReShade.ini`、`wwmi_dx12.ini`、缓存与 `WWMI` 目录（v1.0.2 / v1.0.3 老用户零配置直接可用）。
- 并排升级（保留旧版可回滚）：为新版显式指定新的 `--base-root`（例如 `./wwmi-dx12-setup install --base-root ~/Tools/WWMI-DX12-RT-v1.0.4`）。工具会核验旧根账本、配对宿主 DLL 与 Win64 双文件 SHA-256，自动迁移配置/缓存/第三方 `*.addon64`，默认沿用生效的 `WWMI` Mod 目录并保留旧目录（若旧 `BasePath` 无法自动反推可附带 `--migrate-from <旧根>`）。
- 一键回滚（支持回滚至 v1.0.2 / v1.0.3）：直接在新版工具执行 `./wwmi-dx12-setup rollback --base-root <新根>`，即可恢复旧目录的引导桥与账本。
- 若更换了 `--base-root` 且 Steam 启动选项使用了 `LD_PRELOAD`，请按提示手动同步 `wine-filename-utf8.so` 路径（本工具不会修改 Steam 配置或 XXMI）。
- 下载新版压缩包后可用 `SHA256SUMS.txt` 核验校验和。
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


def resolve_release_version(
    explicit_version: str | None = None,
    repo_root: Path = ROOT,
    commit: str | None = None,
) -> str:
    if explicit_version is not None:
        cleaned = explicit_version.strip()
        if cleaned:
            return cleaned
    if (repo_root / ".git").exists():
        try:
            tag = git_output(repo_root, "describe", "--tags", "--exact-match")
            if tag:
                return tag
        except (OSError, subprocess.CalledProcessError):
            pass
    short_sha = commit[:7] if commit else None
    if not short_sha and (repo_root / ".git").exists():
        try:
            short_sha = git_output(repo_root, "rev-parse", "--short=7", "HEAD")
        except (OSError, subprocess.CalledProcessError):
            short_sha = None
    return f"dev-{short_sha}" if short_sha else "dev"


def build_provenance(manifest: dict | None, version: str | None = None) -> dict:
    commit = manifest.get("repository_commit") if manifest else None
    dirty = manifest.get("working_tree_dirty") if manifest else None
    if commit is None and (ROOT / ".git").exists():
        try:
            commit = git_output(ROOT, "rev-parse", "HEAD")
            dirty = bool(git_output(ROOT, "status", "--porcelain"))
        except (OSError, subprocess.CalledProcessError):
            pass
    resolved_version = resolve_release_version(version, ROOT, commit)
    return {
        "version": resolved_version,
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


def write_sha256sums(archives: Sequence[Path], out_path: Path) -> Path:
    lines = [f"{core.sha256_file(archive)}  {archive.name}\n" for archive in archives]
    out_path.write_text("".join(lines), encoding="utf-8", newline="\n")
    return out_path


def build_asset_manifest_entry(
    src_dir: Path,
    archive_path: Path,
    fmt: str,
    executable_relpaths: set[str] | None = None,
) -> dict:
    exec_set = executable_relpaths or set()
    files: list[dict] = []
    unpacked_bytes = 0
    for p in sorted(src_dir.rglob("*")):
        if not p.is_file():
            continue
        rel = p.relative_to(src_dir).as_posix()
        size = p.stat().st_size
        unpacked_bytes += size
        entry: dict = {
            "path": rel,
            "bytes": size,
            "sha256": core.sha256_file(p),
        }
        if rel in exec_set:
            entry["executable"] = True
        files.append(entry)
    return {
        "name": archive_path.name,
        "format": fmt,
        "root": src_dir.name,
        "bytes": archive_path.stat().st_size,
        "sha256": core.sha256_file(archive_path),
        "unpacked_bytes": unpacked_bytes,
        "file_count": len(files),
        "files": files,
    }


def build_update_manifest(
    prov_obj: dict,
    assets: dict[str, dict],
    published_at: str | None = None,
) -> dict:
    version = str(prov_obj.get("version") or "dev").strip() or "dev"
    commit = str(prov_obj.get("repository_commit") or "").strip()
    pub = published_at or _dt.datetime.now(_dt.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    release_url = (
        f"https://github.com/Bud-Open/wwmi-dx12-rt/releases/tag/{version}"
        if core.parse_semver(version) is not None
        else core.GITHUB_FALLBACK_RELEASE_URL
    )
    return {
        "schema_version": 1,
        "min_updater_protocol": 1,
        "repo": "Bud-Open/wwmi-dx12-rt",
        "version": version,
        "commit": commit,
        "published_at": pub,
        "release_url": release_url,
        "assets": assets,
    }


def write_update_manifest(manifest: dict, out_path: Path) -> Path:
    out_path.write_text(json.dumps(manifest, indent=2, ensure_ascii=False) + "\n", encoding="utf-8", newline="\n")
    return out_path


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
    ap.add_argument(
        "--version",
        default=None,
        help="Explicit release version (e.g. v1.0.5); defaults to exact git tag or dev-<short_sha>",
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
    prov_obj = build_provenance(manifest, version=args.version)
    provenance = json.dumps(prov_obj, ensure_ascii=False, indent=2) + "\n"
    built_archives: list[Path] = []
    manifest_assets: dict[str, dict] = {}

    print(f"Release packages built successfully (version: {prov_obj['version']}):")
    if build_win:
        win_dir = dist_base / "WWMI-DX12-RT-win64"
        win_dir.mkdir(parents=True, exist_ok=True)
        (win_dir / "BUILD-PROVENANCE.json").write_text(provenance, encoding="utf-8")
        populate_common_runtime(win_dir, payload_map, include_linux_so=False)
        shutil.copy2(setup_exe, win_dir / "WWMI-DX12-RT-Setup.exe")
        (win_dir / "README-WINDOWS.txt").write_text(WINDOWS_GUIDE, encoding="utf-8", newline="\r\n")
        win_zip = dist_base / "WWMI-DX12-RT-win64.zip"
        make_zip(win_dir, win_zip)
        built_archives.append(win_zip)
        manifest_assets["win-x64"] = build_asset_manifest_entry(win_dir, win_zip, "zip")
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
        built_archives.append(lin_tar)
        manifest_assets["linux-x64"] = build_asset_manifest_entry(lin_dir, lin_tar, "tar.gz", LINUX_EXECUTABLES)
        print(f"  [Linux]   {lin_tar} ({lin_tar.stat().st_size:,} bytes)")

    if built_archives:
        sums_path = write_sha256sums(built_archives, dist_base / "SHA256SUMS.txt")
        print(f"  [SHA256]  {sums_path}")
        manifest_obj = build_update_manifest(prov_obj, manifest_assets)
        manifest_path = write_update_manifest(manifest_obj, dist_base / "update-manifest.json")
        print(f"  [Manifest] {manifest_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
