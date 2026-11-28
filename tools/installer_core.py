#!/usr/bin/env python3
"""Cross-platform installer, BasePath 2-file mounter, ledger uninstaller, and environment doctor
for WWMI-DX12-RT (Windows Official Kuro Launcher / Windows Steam / Linux & SteamOS Proton).

Hard invariants:
  1. Only writes TWO bootstrap files into the game's Client/Binaries/Win64 directory:
       - dxgi.dll (paired forward host)
       - ReShade.ini (2-line read-only [INSTALL] BasePath=<external_root> redirector)
     All addons, shaders, dxc, wwmi_dx12.ini, caches, and rotated logs stay in <base_root>.
  2. Refuses to overwrite any pre-existing dxgi.dll or ReShade.ini in Win64 unless its path/hash
     matches an active ledger owned by WWMI-DX12-RT.
  3. Never permanently deletes files via unlink/rmtree: uninstall moves ledger-owned files into a
     timestamped staging batch directory with manifest.json (~/.pi/deletion-staging on Linux).
"""
from __future__ import annotations

import ast
import contextlib
import datetime as _dt
import hashlib
import json
import locale
import os
import re
import shlex
import shutil
import stat
import sys
import tempfile
import urllib.error
import urllib.request
import uuid
from dataclasses import dataclass, field
from pathlib import Path, PurePath, PurePosixPath, PureWindowsPath
from typing import Sequence

STEAM_APPID = "3513350"
TARGET_SHIPPING_EXE = "Client-Win64-Shipping.exe"
BOOTSTRAP_FILES = ("dxgi.dll", "ReShade.ini")
ACTIVE_STATES = ("installed", "installing", "rolling_back")
PENDING_STATES = ("installing", "rolling_back")
REQUIRED_PAYLOAD = (
    "host/ReShade64-game-forward.dll",
    "reshade-addons/wwmi_dx12.addon64",
    "reshade-addons/zz_wwmi_rt_pack.addon64",
    *(f"reshade-addons/wwmi_dx12/{name}" for name in (
        "dxc/dxcompiler.dll", "dxc/dxil.dll", "Bc7Half.cso", "BufCopy.cso", "IbVbHash.cso",
    )),
    *(f"reshade-addons/rt-pack-data/{name}.cso" for name in (
        "CopyFinalPositions", "NativeDiscoveryCopy", "PatchInstances", "PatchSbt",
        "ProbeRtInstanceRows", "ReadRtRaygenId", "ShapePositions", "ShapeTangents",
        "SkinPositions", "SkinTangents",
    )),
)
WATCH_PROCESS_NAMES = (
    "Client-Win64-Shipping.exe",
    "Wuthering Waves.exe",
)
WWMI_OWNED_ADDON_FILES = frozenset(
    Path(rel).name.lower()
    for rel in REQUIRED_PAYLOAD
    if rel.startswith("reshade-addons/") and Path(rel).suffix.lower() in (".addon", ".addon64")
)
COMPATIBLE_CACHE_SUFFIXES = frozenset({".cache", ".texlook", ".lodcache", ".bin"})
_SEMVER_RE = re.compile(
    r"^[vV]?(\d+)\.(\d+)\.(\d+)(?:-([0-9A-Za-z.-]+))?(?:\+[0-9A-Za-z.-]+)?$"
)
_WATCH_PROC_PATTERNS = tuple(
    (re.compile(rb'(?:^|[/"])' + re.escape(w.encode("utf-8").lower()) + rb'(?:$|[\s"])'), w)
    for w in WATCH_PROCESS_NAMES
)


def parse_semver(version: str | None) -> tuple[int, int, int, tuple[tuple[int, int | str], ...]] | None:
    """Parse SemVer string like 'v1.0.5' or '1.2.0-rc.1'; returns None for non-SemVer (e.g. 'dev')."""
    if not isinstance(version, str):
        return None
    m = _SEMVER_RE.match(version.strip())
    if not m:
        return None
    major, minor, patch = int(m.group(1)), int(m.group(2)), int(m.group(3))
    pre_raw = m.group(4)
    if not pre_raw:
        return (major, minor, patch, ())
    pre_parts: list[tuple[int, int | str]] = []
    for part in pre_raw.split("."):
        if not part:
            return None
        if part.isdigit():
            pre_parts.append((0, int(part)))
        else:
            pre_parts.append((1, part))
    return (major, minor, patch, tuple(pre_parts))


def compare_semver(a: str | None, b: str | None) -> int | None:
    """Compare two SemVer strings: -1 if a < b, 0 if equal, 1 if a > b, None if either invalid."""
    pa, pb = parse_semver(a), parse_semver(b)
    if pa is None or pb is None:
        return None
    if pa[:3] != pb[:3]:
        return -1 if pa[:3] < pb[:3] else 1
    pre_a, pre_b = pa[3], pb[3]
    if pre_a == pre_b:
        return 0
    if not pre_a:
        return 1  # Release > prerelease
    if not pre_b:
        return -1
    return -1 if pre_a < pre_b else 1


def is_newer_version(current_version: str | None, latest_version: str | None) -> bool:
    return compare_semver(current_version, latest_version) == -1


GITHUB_RELEASE_API_URL = "https://api.github.com/repos/Bud-Open/wwmi-dx12-rt/releases/latest"
GITHUB_ALLOWED_URL_PREFIX = "https://github.com/Bud-Open/wwmi-dx12-rt/"
GITHUB_FALLBACK_RELEASE_URL = "https://github.com/Bud-Open/wwmi-dx12-rt/releases/latest"


def is_allowed_github_release_url(url: str | None) -> bool:
    if not isinstance(url, str):
        return False
    if not url.startswith(GITHUB_ALLOWED_URL_PREFIX):
        return False
    return not any(c.isspace() or ord(c) < 32 or c in ('"', "'") for c in url)


def parse_github_release_payload(
    data: dict,
    current_version: str | None,
    platform_kind: str | None = None,
) -> dict:
    """Parse GitHub releases/latest JSON into a structured, URL-validated update check result."""
    if not isinstance(data, dict):
        raise ValueError("GitHub Release 响应格式无效")
    p_kind = platform_kind or ("windows" if sys.platform == "win32" else "linux-proton")
    latest_version = str(data.get("tag_name") or "").strip()
    raw_html_url = data.get("html_url")
    release_url = raw_html_url if is_allowed_github_release_url(raw_html_url) else GITHUB_FALLBACK_RELEASE_URL
    published_at = str(data.get("published_at") or "").strip()

    preferred_suffix = "-win64.zip" if p_kind == "windows" else "-linux.tar.gz"
    fallback_suffix = ".zip" if p_kind == "windows" else ".tar.gz"
    selected_asset: dict | None = None
    assets = data.get("assets")
    if isinstance(assets, list):
        for suffix in (preferred_suffix, fallback_suffix):
            for item in assets:
                if not isinstance(item, dict):
                    continue
                name = str(item.get("name") or "")
                if name.lower().endswith(suffix):
                    selected_asset = item
                    break
            if selected_asset is not None:
                break

    asset_name: str | None = None
    asset_url: str | None = None
    asset_digest: str | None = None
    if selected_asset is not None:
        raw_name = str(selected_asset.get("name") or "").strip()
        if raw_name:
            asset_name = raw_name
        raw_dl = selected_asset.get("browser_download_url")
        if is_allowed_github_release_url(raw_dl):
            asset_url = raw_dl
        raw_digest = selected_asset.get("digest")
        if isinstance(raw_digest, str) and raw_digest.strip():
            asset_digest = raw_digest.strip()

    cur_str = (current_version or "dev").strip() or "dev"
    return {
        "current_version": cur_str,
        "latest_version": latest_version,
        "update_available": is_newer_version(cur_str, latest_version),
        "release_url": release_url,
        "asset_name": asset_name,
        "asset_url": asset_url,
        "asset_digest": asset_digest,
        "published_at": published_at,
    }


def check_latest_release(
    current_version: str | None,
    platform_kind: str | None = None,
    *,
    timeout: float = 8.0,
    urlopen_fn=None,
) -> dict:
    """Opt-in manual HTTPS check against GitHub Releases API (never called automatically)."""
    req = urllib.request.Request(
        GITHUB_RELEASE_API_URL,
        headers={
            "Accept": "application/vnd.github+json",
            "User-Agent": "WWMI-DX12-RT-Setup",
        },
        method="GET",
    )
    opener = urlopen_fn or urllib.request.urlopen
    try:
        with opener(req, timeout=timeout) as resp:
            raw = resp.read(1 << 20)
    except (OSError, urllib.error.URLError, TimeoutError) as exc:
        raise RuntimeError(tr(
            f"无法连接 GitHub 检查更新：{exc}",
            f"Failed to contact GitHub Releases API: {exc}",
        )) from exc
    try:
        data = json.loads(raw.decode("utf-8"))
        return parse_github_release_payload(data, current_version, platform_kind)
    except (UnicodeDecodeError, ValueError) as exc:
        raise RuntimeError(tr(
            f"解析 GitHub 更新信息失败：{exc}",
            f"Failed to parse GitHub release metadata: {exc}",
        )) from exc


def is_chinese_locale() -> bool:
    override = os.environ.get("WWMI_SETUP_LANG", "").strip().lower()
    if override.startswith("en"):
        return False
    if override.startswith("zh"):
        return True
    for key in ("LC_ALL", "LC_MESSAGES", "LANG"):
        val = os.environ.get(key, "").strip().lower()
        if val:
            return val.startswith("zh")
    loc = (locale.getlocale()[0] or "").strip().lower()
    return loc.startswith("zh") or "chinese" in loc


def tr(zh: str, en: str) -> str:
    return zh if is_chinese_locale() else en


@dataclass
class GameInstall:
    label: str
    platform_kind: str  # "windows" | "linux-proton"
    game_root: Path
    win64_dir: Path
    launcher_root: Path | None = None
    compatdata_dir: Path | None = None
    is_flatpak_steam: bool = False


@dataclass
class DoctorFinding:
    level: str  # "ok" | "warn" | "error"
    code: str
    message: str


@dataclass
class DoctorReport:
    win64_dir: str
    wwmi_dir: str
    base_root: str
    mount_state: str
    rabbitfx_count: int = 0
    non_ascii_mods: list[str] = field(default_factory=list)
    findings: list[DoctorFinding] = field(default_factory=list)
    package_version: str = "dev"
    installed_version: str | None = None
    installed_commit: str | None = None

    @property
    def healthy(self) -> bool:
        return not any(f.level == "error" for f in self.findings)


def read_package_provenance(root: Path) -> dict[str, str | None]:
    """Read package version and repository_commit from BUILD-PROVENANCE.json if present."""
    result: dict[str, str | None] = {"version": "dev", "repository_commit": None}
    prov_path = root / "BUILD-PROVENANCE.json"
    try:
        if not _regular_file(prov_path):
            return result
        data = json.loads(prov_path.read_text(encoding="utf-8"))
        if not isinstance(data, dict):
            return result
    except (OSError, RuntimeError, ValueError):
        return result
    ver = data.get("version")
    if isinstance(ver, str) and ver.strip():
        result["version"] = ver.strip()
    commit = data.get("repository_commit")
    if isinstance(commit, str) and commit.strip():
        result["repository_commit"] = commit.strip()
    return result


def format_installed_version(ledger: dict | None) -> str:
    """Format ledger installed_version, labeling pre-v1.0.5 ledgers cleanly as legacy/unknown."""
    if isinstance(ledger, dict):
        ver = ledger.get("installed_version")
        if isinstance(ver, str) and ver.strip():
            return ver.strip()
    return tr("旧版（版本未知）", "Legacy (version unknown)")


def sha256_file(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        while True:
            chunk = f.read(1 << 20)
            if not chunk:
                break
            h.update(chunk)
    return h.hexdigest()


def format_engine_path(path: PurePath, platform_kind: str) -> str:
    """Format an absolute host path as seen inside the game."""
    if isinstance(path, Path):
        path = path.resolve()
    raw = str(path).replace("/", "\\")
    if platform_kind == "windows" or path.drive:
        return raw
    # Linux and SteamOS Wine/Proton map host '/' to Z:\.
    return "Z:" + raw


def parse_vdf_library_paths(vdf_text: str) -> list[str]:
    """Extract all `"path" "..."` values from Steam's libraryfolders.vdf."""
    out: list[str] = []
    for m in re.finditer(r'"path"\s+"([^"]+)"', vdf_text, flags=re.IGNORECASE):
        val = m.group(1).replace("\\\\", "\\").strip()
        if val and val not in out:
            out.append(val)
    return out


def _probe_game_root_candidates(root: Path) -> tuple[Path, Path] | None:
    """Given an outer install directory, locate (game_root, win64_dir) if valid."""
    for sub in (root / "Wuthering Waves Game", root):
        win64 = sub / "Client" / "Binaries" / "Win64"
        if (win64 / TARGET_SHIPPING_EXE).is_file():
            return sub, win64
    return None


def discover_linux_game_installs(home: Path | None = None) -> list[GameInstall]:
    home_dir = (home or Path.home()).resolve()
    steam_roots = [
        (home_dir / ".local/share/Steam", False, "Steam (Native)"),
        (home_dir / ".steam/steam", False, "Steam (Symlink)"),
        (
            home_dir / ".var/app/com.valvesoftware.Steam/.local/share/Steam",
            True,
            "Steam (Flatpak)",
        ),
    ]
    seen_win64: set[Path] = set()
    installs: list[GameInstall] = []

    for steam_root, is_flatpak, label_prefix in steam_roots:
        if not steam_root.is_dir():
            continue
        libs: list[Path] = [steam_root]
        vdf_path = steam_root / "steamapps/libraryfolders.vdf"
        if vdf_path.is_file():
            try:
                for p_str in parse_vdf_library_paths(vdf_path.read_text(encoding="utf-8", errors="ignore")):
                    p = Path(p_str)
                    if p.is_dir() and p not in libs:
                        libs.append(p)
            except OSError:
                pass

        for lib in libs:
            outer = lib / "steamapps/common/Wuthering Waves"
            hit = _probe_game_root_candidates(outer)
            if not hit:
                continue
            game_root, win64 = hit
            win64_real = win64.resolve()
            if win64_real in seen_win64:
                continue
            seen_win64.add(win64_real)
            compat = lib / "steamapps/compatdata" / STEAM_APPID
            installs.append(
                GameInstall(
                    label=f"{label_prefix}: {game_root}",
                    platform_kind="linux-proton",
                    game_root=game_root,
                    win64_dir=win64_real,
                    launcher_root=outer if (outer / "launcher.exe").is_file() else None,
                    compatdata_dir=compat if compat.is_dir() else None,
                    is_flatpak_steam=is_flatpak,
                )
            )
    return installs


def discover_windows_game_installs(extra_roots: Sequence[Path] = ()) -> list[GameInstall]:
    candidates: list[tuple[str, Path]] = []

    # 1. Registry lookup on Windows (Official Kuro Launcher + Steam)
    if sys.platform == "win32":
        try:
            import winreg  # type: ignore

            uninstall_keys = [
                (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Microsoft\Windows\CurrentVersion\Uninstall"),
                (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\Microsoft\Windows\CurrentVersion\Uninstall"),
                (winreg.HKEY_CURRENT_USER, r"Software\Microsoft\Windows\CurrentVersion\Uninstall"),
            ]
            for hive, subkey in uninstall_keys:
                try:
                    with winreg.OpenKey(hive, subkey) as k:
                        i = 0
                        while True:
                            try:
                                name = winreg.EnumKey(k, i)
                                i += 1
                            except OSError:
                                break
                            if not any(w in name.lower() for w in ("wuthering", "kuro", "鸣潮", "3513350")):
                                continue
                            try:
                                with winreg.OpenKey(k, name) as sk:
                                    loc, _ = winreg.QueryValueEx(sk, "InstallLocation")
                                    if isinstance(loc, str) and loc.strip():
                                        candidates.append(("Kuro Official / Registry", Path(loc.strip())))
                            except OSError:
                                pass
                except OSError:
                    pass

            for hive, subkey, val_name in (
                (winreg.HKEY_CURRENT_USER, r"Software\Valve\Steam", "SteamPath"),
                (winreg.HKEY_LOCAL_MACHINE, r"SOFTWARE\WOW6432Node\Valve\Steam", "InstallPath"),
            ):
                try:
                    with winreg.OpenKey(hive, subkey) as sk:
                        sp, _ = winreg.QueryValueEx(sk, val_name)
                        if isinstance(sp, str) and sp.strip():
                            sroot = Path(sp.strip())
                            candidates.append(("Steam", sroot / "steamapps/common/Wuthering Waves"))
                            vdf = sroot / "steamapps/libraryfolders.vdf"
                            if vdf.is_file():
                                for lp in parse_vdf_library_paths(vdf.read_text(encoding="utf-8", errors="ignore")):
                                    candidates.append(("Steam Library", Path(lp) / "steamapps/common/Wuthering Waves"))
                except OSError:
                    pass
        except ImportError:
            pass

        # Common drive folder names
        for drv in "CDEFGHIJ":
            drive = Path(f"{drv}:\\")
            if not drive.exists():
                continue
            for rel in (
                "Wuthering Waves",
                "Games\\Wuthering Waves",
                "Program Files\\Wuthering Waves",
                "Program Files (x86)\\Steam\\steamapps\\common\\Wuthering Waves",
                "SteamLibrary\\steamapps\\common\\Wuthering Waves",
            ):
                candidates.append(("Disk Scan", drive / rel))

    for r in extra_roots:
        candidates.append(("Custom", r))

    seen: set[Path] = set()
    installs: list[GameInstall] = []
    for src_label, outer in candidates:
        hit = _probe_game_root_candidates(outer)
        if not hit:
            continue
        game_root, win64 = hit
        w_res = win64.resolve()
        if w_res in seen:
            continue
        seen.add(w_res)
        installs.append(
            GameInstall(
                label=f"{src_label}: {game_root}",
                platform_kind="windows",
                game_root=game_root,
                win64_dir=w_res,
                launcher_root=outer if (outer / "launcher.exe").is_file() else None,
            )
        )
    return installs


def discover_game_installs(home: Path | None = None) -> list[GameInstall]:
    if sys.platform == "win32":
        return discover_windows_game_installs()
    return discover_linux_game_installs(home=home)


def select_default_linux_base_root(install: GameInstall | None, home: Path | None = None) -> Path:
    """Choose XDG base root while automatically adapting to Flatpak Steam sandbox visibility."""
    home_dir = (home or Path.home()).resolve()
    if install and install.is_flatpak_steam:
        flatpak_data = home_dir / ".var/app/com.valvesoftware.Steam/data"
        if flatpak_data.is_dir():
            return flatpak_data / "wwmi-dx12-rt"
        if install.compatdata_dir:
            return install.compatdata_dir / "wwmi-dx12-rt"
    xdg_data = os.environ.get("XDG_DATA_HOME", "").strip()
    if xdg_data and home is None:
        return Path(xdg_data).resolve() / "wwmi-dx12-rt"
    return home_dir / ".local/share/wwmi-dx12-rt"


def discover_wwmi_dirs(home: Path | None = None, install: GameInstall | None = None) -> list[Path]:
    home_dir = (home or Path.home()).resolve()
    candidates: list[Path] = []
    use_windows = (install.platform_kind == "windows") if install is not None else (sys.platform == "win32")
    if use_windows:
        appdata = os.environ.get("APPDATA", "")
        localappdata = os.environ.get("LOCALAPPDATA", "")
        if appdata:
            candidates.append(Path(appdata) / "XXMI Launcher/WWMI")
        if localappdata:
            candidates.append(Path(localappdata) / "XXMI Launcher/WWMI")
        for drv in "CDEFGHIJ":
            d = Path(f"{drv}:\\")
            if d.exists():
                candidates.extend([d / "XXMI/WWMI", d / "Games/XXMI/WWMI", d / "Tools/XXMI/WWMI"])
    elif install and install.is_flatpak_steam:
        # Flatpak Steam cannot read arbitrary host paths like ~/.local/share/XXMI-Launcher by default.
        fp_base = select_default_linux_base_root(install, home=home_dir)
        candidates.append(fp_base / "WWMI")
        candidates.append(home_dir / ".var/app/com.valvesoftware.Steam/data/XXMI-Launcher/WWMI")
        if install.compatdata_dir:
            candidates.append(install.compatdata_dir / "WWMI")
    else:
        candidates.extend(
            [
                home_dir / ".local/share/XXMI-Launcher/WWMI",
                home_dir / "Games/XXMI/WWMI",
                home_dir / ".local/share/wwmi-dx12-rt/WWMI",
            ]
        )

    found: list[Path] = []
    seen: set[Path] = set()
    for c in candidates:
        if (c / "d3dx.ini").is_file():
            r = c.resolve()
            if r not in seen:
                seen.add(r)
                found.append(r)
    return found


def init_wwmi_skeleton(target_dir: Path) -> Path:
    """Create a clean standard WWMI directory skeleton for users who do not use XXMI Launcher."""
    target = target_dir.resolve()
    (target / "Mods").mkdir(parents=True, exist_ok=True)
    (target / "Core").mkdir(parents=True, exist_ok=True)
    (target / "ShaderFixes").mkdir(parents=True, exist_ok=True)
    d3dx_ini = target / "d3dx.ini"
    if not d3dx_ini.exists():
        d3dx_ini.write_text(
            "; Standard WWMI directory initialized by WWMI-DX12-RT\n"
            "[Include]\n"
            "include = Core\\WWMI\\WuWa-Model-Importer.ini\n"
            "include_recursive = Mods\n"
            "exclude_recursive = DISABLED*\n"
            "exclude_recursive = desktop.ini\n\n"
            "[Hunting]\n"
            "hunting = 0\n\n"
            "[KeyReload]\n"
            "Key = no_modifiers VK_F10\n"
            "type = reload_config\n",
            encoding="utf-8",
        )
    return target


def running_watched_processes(proc_root: Path = Path("/proc")) -> list[str]:
    """Detect running Wuthering Waves executables without killing anything."""
    hits: list[str] = []
    if sys.platform == "win32":
        import subprocess

        try:
            res = subprocess.run(
                ["tasklist", "/FO", "CSV", "/NH"], capture_output=True, timeout=5, check=True,
            )
            for line in res.stdout.splitlines():
                low = line.lower()
                for w in WATCH_PROCESS_NAMES:
                    if w.lower().encode("ascii") in low and w not in hits:
                        hits.append(w)
        except (OSError, subprocess.SubprocessError) as exc:
            raise RuntimeError("无法检查游戏进程；请确认已退出游戏再重试") from exc
        return hits

    if not proc_root.is_dir():
        return hits
    self_pid = str(os.getpid())
    for pid in proc_root.glob("[0-9]*"):
        if pid.name == self_pid:
            continue
        try:
            raw_cmdline = (pid / "cmdline").read_bytes()
        except OSError:
            continue
        if not raw_cmdline:
            continue
        matched: str | None = None
        for token in raw_cmdline.split(b"\0"):
            if not token:
                continue
            norm = token.replace(b"\\", b"/").lower()
            for pat, proc_name in _WATCH_PROC_PATTERNS:
                if pat.search(norm):
                    matched = proc_name
                    break
            if matched:
                break
        if matched:
            hits.append(f"{pid.name}:{matched}")
    return hits


def scan_mods_health(wwmi_dir: Path) -> tuple[list[Path], list[str]]:
    """Scan <wwmi_dir>/Mods (skipping DISABLED*) for RabbitFX.ini instances and non-ASCII mod folder names."""
    mods_root = wwmi_dir / "Mods"
    rabbitfx_hits: list[Path] = []
    non_ascii_names: list[str] = []
    if not mods_root.is_dir():
        return rabbitfx_hits, non_ascii_names

    def walk(cur: Path, depth: int) -> None:
        if depth > 6:
            return
        try:
            entries = sorted(cur.iterdir(), key=lambda p: p.name.lower())
        except OSError:
            return
        for entry in entries:
            if entry.name.upper().startswith("DISABLED"):
                continue
            if depth <= 2 and any(ord(ch) > 127 for ch in entry.name):
                rel = entry.relative_to(mods_root).as_posix()
                if rel not in non_ascii_names:
                    non_ascii_names.append(rel)
            if entry.is_dir():
                walk(entry, depth + 1)
            elif entry.is_file() and entry.name.lower() == "rabbitfx.ini":
                rabbitfx_hits.append(entry)

    walk(mods_root, 1)
    return rabbitfx_hits, non_ascii_names


def _regular_file(path: Path) -> bool:
    """Missing is False; an unreadable path, symlink or other special file is an error."""
    try:
        st = path.lstat()
    except FileNotFoundError:
        return False
    if (not stat.S_ISREG(st.st_mode) or
            getattr(st, "st_file_attributes", 0) & stat.FILE_ATTRIBUTE_REPARSE_POINT):
        raise RuntimeError(f"拒绝操作非普通文件（含链接/重解析点）：{path}")
    return True


def _ledger_path(base_root: Path) -> Path:
    return base_root / "runtime/install-ledger.json"


def _reject_duplicate_keys(pairs: list[tuple[str, object]]) -> dict:
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"重复账本字段：{key}")
        result[key] = value
    return result


def _reject_json_constant(token: str):
    raise ValueError(f"无效 JSON 常量：{token}")


def load_install_ledger(base_root: Path) -> dict | None:
    path = _ledger_path(base_root)
    try:
        if not _regular_file(path):
            return None
        ledger = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=_reject_duplicate_keys,
                            parse_constant=_reject_json_constant)
        if type(ledger["schema_version"]) is not int or ledger["schema_version"] != 1:
            raise ValueError("不支持的账本版本")
        target = Path(ledger["win64_dir"])
        if not target.is_absolute() or ".." in target.parts or "\0" in str(target):
            raise ValueError("win64_dir 必须为绝对路径且不得包含 '..'")
        if ledger["state"] in ACTIVE_STATES:
            records = ledger["win64_files"]
            if not isinstance(records, list) or len(records) != len(BOOTSTRAP_FILES):
                raise ValueError("引导文件清单不完整")
            names = set()
            for item in records:
                name = item["relative"]
                if name not in BOOTSTRAP_FILES or name in names or Path(item["path"]) != target / name:
                    raise ValueError("引导文件路径不在 Win64 白名单内")
                names.add(name)
                for key in ("sha256", "previous_sha256"):
                    if key == "previous_sha256" and (ledger["state"] not in PENDING_STATES or (ledger["state"] == "installing" and key not in item)):
                        continue
                    if not re.fullmatch(r"[0-9a-f]{64}", item[key]):
                        raise ValueError(f"{name} 的 {key} 无效")
            if ledger["state"] in PENDING_STATES:
                if not re.fullmatch(r"[0-9a-f]{32}", ledger["pending_id"]):
                    raise ValueError("安装事务标识无效")
                if ledger["state"] == "installing":
                    prev_owner = ledger.get("previous_owner")
                    if prev_owner is not None:
                        po = Path(prev_owner)
                        if not po.is_absolute() or ".." in po.parts or "\0" in str(po):
                            raise ValueError("previous_owner 路径无效")
                else:
                    rb_target = ledger.get("rollback_target")
                    if not isinstance(rb_target, str) or not rb_target.strip():
                        raise ValueError("rolling_back 账本缺少 rollback_target")
                    rt_path = Path(rb_target)
                    if not rt_path.is_absolute() or ".." in rt_path.parts or "\0" in str(rt_path):
                        raise ValueError("rollback_target 路径无效")
                rec_root = (base_root.resolve() / "runtime/recovery").resolve()
                for item in records:
                    rb = item.get("recovery_backup")
                    if ledger["state"] == "rolling_back" and rb is None:
                        raise ValueError("rolling_back 账本缺少 recovery_backup")
                    if rb is not None:
                        rbp = Path(rb)
                        if (not rbp.is_absolute() or ".." in rbp.parts or "\0" in str(rbp)
                                or not rbp.resolve().is_relative_to(rec_root)):
                            raise ValueError("recovery_backup 路径不在允许的恢复目录内")
        elif ledger["state"] != "uninstalled":
            raise ValueError("未知账本状态")
        return ledger
    except (AttributeError, KeyError, OSError, RuntimeError, TypeError, ValueError) as exc:
        raise RuntimeError(
            tr(
                f"安装账本损坏或无法读取：{path}。请从备份恢复账本；无备份时先人工核对原 Win64 引导文件归属。（{exc}）",
                f"Install ledger is corrupted or unreadable: {path}. Restore from backup or inspect your Win64 bootstrap files manually. ({exc})",
            )
        ) from exc


def _verified_recovery_backup_path(rec: dict, base_root: Path | None) -> Path | None:
    """Verify rec['recovery_backup'] is a regular file under base_root/runtime/recovery matching previous_sha256."""
    prev_sha = rec.get("previous_sha256")
    bkp_raw = rec.get("recovery_backup")
    if not isinstance(prev_sha, str) or not isinstance(bkp_raw, str) or not bkp_raw.strip():
        return None
    try:
        bkp = Path(bkp_raw).resolve()
        if base_root is not None:
            rec_root = (base_root.resolve() / "runtime/recovery").resolve()
            if not bkp.is_relative_to(rec_root):
                return None
        if not _regular_file(bkp):
            return None
        if sha256_file(bkp) != prev_sha:
            return None
        return bkp
    except (OSError, RuntimeError):
        return None


def _owned_bootstrap(win64_dir: Path, ledger: dict | None, base_root: Path | None = None) -> dict[str, str]:
    records = {item["relative"]: item for item in ledger["win64_files"]} if ledger else {}
    eff_base = base_root
    if eff_base is None and ledger and isinstance(ledger.get("base_root"), str) and ledger["base_root"].strip():
        eff_base = Path(ledger["base_root"])
    owned = {}
    for name in BOOTSTRAP_FILES:
        path = win64_dir / name
        if not _regular_file(path):
            continue
        if not ledger:
            raise RuntimeError(
                tr(
                    f"Win64/{name} 已存在，但没有对应的活动账本，拒绝覆盖"
                    "（若已有其他 ReShade/DXGI，请先备份移走原 Win64/dxgi.dll 与 ReShade.ini 后重试；"
                    "WWMI-DX12-RT 需使用配对宿主，第三方 .addon64 如 RenoDX 可放入外部 reshade-addons/ 或在外部 ReShade.ini 设 LoadGameDirAddons=1 共存。"
                    "若为旧版换目录升级，请先在旧目录执行卸载，或覆盖原目录并保留 runtime/install-ledger.json 后重装）",
                    f"Win64/{name} already exists without an active WWMI-DX12-RT ledger; refusing to overwrite. "
                    f"If you already use ReShade/DXGI, back up and move Win64/{name} and Win64/ReShade.ini first, then retry Install. "
                    "WWMI-DX12-RT requires its paired dxgi.dll; to keep third-party .addon64 add-ons (e.g. RenoDX), place them in "
                    "<ExternalFolder>/reshade-addons/ or set LoadGameDirAddons=1 under [ADDON] in <ExternalFolder>/ReShade.ini.",
                )
            )
        sha = sha256_file(path)
        rec = records[name]
        previous = None
        if ledger["state"] in PENDING_STATES and "previous_sha256" in rec:
            if ledger["state"] == "rolling_back" or "recovery_backup" in rec or ledger.get("previous_owner"):
                if _verified_recovery_backup_path(rec, eff_base) is not None:
                    previous = rec["previous_sha256"]
            else:
                previous = rec["previous_sha256"]
        if sha not in (rec["sha256"], previous):
            raise RuntimeError(
                tr(
                    f"Win64/{name} 哈希与账本不一致，拒绝修改",
                    f"Win64/{name} hash does not match the install ledger; refusing to modify.",
                )
            )
        owned[name] = sha
    return owned


def parse_win64_stub_base_path(text: str, *, strict: bool = False) -> str | None:
    """Parse [INSTALL] BasePath=<raw> from Win64/ReShade.ini.

    When strict=True, reject files containing any non-comment section other than [INSTALL]
    or any key other than a single BasePath entry.
    """
    if text.startswith("\ufeff"):
        text = text[1:]
    in_install = False
    found: str | None = None
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith((";", "#", "/")):
            continue
        sec = _parse_ini_section_name(line)
        if sec is not None:
            if strict and sec != "INSTALL":
                return None
            in_install = sec == "INSTALL"
            continue
        if "=" in line:
            key, _, val = line.partition("=")
            k = key.strip()
            if in_install and k == "BasePath":
                cleaned = val.strip().strip('"')
                if not cleaned or (strict and found is not None):
                    return None
                found = cleaned
                if not strict:
                    return found
                continue
        if strict:
            return None
    return found


def _is_local_windows_drive(drive_letter: str) -> bool:
    """Reject Windows remote/network/unknown drive letters BEFORE filesystem resolution (matches Win32 GetDriveTypeW)."""
    if sys.platform != "win32":
        return True
    try:
        import ctypes
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.GetDriveTypeW.argtypes = [ctypes.c_wchar_p]
        kernel32.GetDriveTypeW.restype = ctypes.c_uint32
        dtype = int(kernel32.GetDriveTypeW(f"{drive_letter.upper()}:\\"))
        # Reject DRIVE_UNKNOWN (0), DRIVE_NO_ROOT_DIR (1), and DRIVE_REMOTE (4)
        return dtype not in (0, 1, 4)
    except OSError:
        return False


def resolve_engine_path_to_host(raw: str | None, platform_kind: str) -> Path | None:
    """Convert an engine-visible path back to a verified local host Path (rejects UNC/remote paths)."""
    if not raw:
        return None
    cleaned = raw.strip().strip('"')
    if not cleaned or "\0" in cleaned or "\n" in cleaned or "\r" in cleaned:
        return None
    if platform_kind == "windows":
        if cleaned.startswith(("\\\\", "//")):
            return None
        if len(cleaned) < 3 or not cleaned[0].isalpha() or cleaned[1] != ":" or cleaned[2] not in ("\\", "/"):
            return None
        pw = PureWindowsPath(cleaned)
        if not pw.is_absolute() or ".." in pw.parts:
            return None
        if not _is_local_windows_drive(cleaned[0]):
            return None
        return Path(cleaned).resolve()
    # Linux/Proton: ONLY reverse-map verifiable Z:\... host paths; never guess arbitrary drive letters.
    if len(cleaned) >= 3 and cleaned[0] in ("Z", "z") and cleaned[1] == ":" and cleaned[2] in ("\\", "/"):
        posix = cleaned[2:].replace("\\", "/")
        pp = PurePosixPath(posix)
        if not pp.is_absolute() or ".." in pp.parts:
            return None
        return Path(posix).resolve()
    return None


@dataclass
class VerifiedWin64Owner:
    base_root: Path
    ledger: dict
    owned_hashes: dict[str, str]
    raw_stub_base_path: str | None = None


def detect_verified_win64_owner(
    win64_dir: Path,
    current_base_root: Path,
    platform_kind: str | None = None,
    migrate_from: Path | None = None,
) -> tuple[VerifiedWin64Owner | None, str | None]:
    """Detect and cryptographically verify an existing side-by-side WWMI-DX12-RT installation in Win64."""
    p_kind = platform_kind or ("windows" if sys.platform == "win32" else "linux-proton")
    win64_res = win64_dir.resolve()
    cur_base_res = current_base_root.resolve()

    present = {name: _regular_file(win64_res / name) for name in BOOTSTRAP_FILES}
    if not any(present.values()):
        return None, None

    raw_base: str | None = None
    parsed_base: Path | None = None
    stub_is_strict = False
    if present["ReShade.ini"]:
        try:
            stub_text = (win64_res / "ReShade.ini").read_text(encoding="utf-8", errors="ignore")
            raw_base = parse_win64_stub_base_path(stub_text, strict=True)
            if raw_base is not None:
                stub_is_strict = True
                parsed_base = resolve_engine_path_to_host(raw_base, p_kind)
            else:
                raw_base = parse_win64_stub_base_path(stub_text, strict=False)
        except OSError:
            raw_base = None
            parsed_base = None

    candidate: Path | None = None
    if migrate_from is not None:
        explicit_a = migrate_from.resolve()
        if present["ReShade.ini"] and not stub_is_strict:
            return None, tr(
                "Win64/ReShade.ini 不是 WWMI-DX12-RT 的 2 行引导文件（可能为第三方 ReShade 配置），拒绝接管。",
                "Win64/ReShade.ini is not a minimal WWMI-DX12-RT bootstrap stub (may belong to third-party ReShade); refusing takeover.",
            )
        if parsed_base is not None and parsed_base != explicit_a:
            return None, tr(
                f"Win64/ReShade.ini 指向 {parsed_base}，与 --migrate-from 指定的 {explicit_a} 不一致，拒绝接管。",
                f"Win64/ReShade.ini points to {parsed_base}, which does not match --migrate-from {explicit_a}; refusing takeover.",
            )
        candidate = explicit_a
    elif parsed_base is not None and stub_is_strict:
        candidate = parsed_base
    else:
        if raw_base and stub_is_strict:
            return None, tr(
                f"Win64/ReShade.ini 中的 BasePath={raw_base} 无法自动反推宿主机目录；"
                "若确认为旧版 WWMI-DX12-RT，请通过 --migrate-from <旧外部目录> 显式指定以核验账本与哈希。",
                f"BasePath={raw_base} in Win64/ReShade.ini cannot be reverse-mapped automatically; "
                "if this is an older WWMI-DX12-RT install, pass --migrate-from <old_base_root> to verify its ledger and hashes.",
            )
        return None, None

    if candidate == cur_base_res:
        return None, None

    if not candidate.is_dir():
        return None, tr(
            f"Win64/ReShade.ini 指向的旧目录不存在：{candidate}；拒绝盲目覆盖。",
            f"Previous directory referenced by Win64/ReShade.ini does not exist: {candidate}; refusing to overwrite.",
        )

    try:
        ledger_a = load_install_ledger(candidate)
    except RuntimeError as exc:
        return None, tr(
            f"无法验证旧目录 {candidate} 的安装账本，拒绝覆盖：{exc}",
            f"Could not verify install ledger in {candidate}; refusing to overwrite: {exc}",
        )

    if not ledger_a:
        return None, tr(
            f"旧目录 {candidate} 缺少有效的 runtime/install-ledger.json，拒绝盲目覆盖。",
            f"Previous directory {candidate} has no runtime/install-ledger.json; refusing to overwrite.",
        )

    state_a = ledger_a.get("state")
    if state_a == "installing":
        return None, tr(
            f"旧目录 {candidate} 上次安装未完成（state=installing）；请先在旧目录完成修复或卸载后再升级。",
            f"Previous installation in {candidate} was interrupted (state=installing); finish repairing or uninstalling it in the old folder first.",
        )
    if state_a != "installed":
        return None, tr(
            f"旧目录 {candidate} 的账本状态为 {state_a}（非已安装状态），拒绝接管。",
            f"Ledger in {candidate} has state={state_a} (not installed); refusing takeover.",
        )

    if Path(ledger_a["win64_dir"]).resolve() != win64_res:
        return None, tr(
            f"旧目录 {candidate} 的账本绑定在 {ledger_a['win64_dir']}，与目标 Win64 目录不匹配，拒绝接管。",
            f"Ledger in {candidate} is bound to {ledger_a['win64_dir']}, which does not match target Win64; refusing takeover.",
        )

    missing = [name for name in BOOTSTRAP_FILES if not present[name]]
    if missing:
        joined = ", ".join(missing)
        return None, tr(
            f"旧安装 {candidate} 的 Win64 引导文件不完整（缺少 {joined}）；请先在旧目录修复或卸载后再升级。",
            f"Existing install in {candidate} is missing Win64 bootstrap file(s) ({joined}); repair or uninstall it in the old folder first.",
        )

    records_a = {item["relative"]: item for item in ledger_a["win64_files"]}
    owned_hashes: dict[str, str] = {}
    for name in BOOTSTRAP_FILES:
        actual_sha = sha256_file(win64_res / name)
        if actual_sha != records_a[name]["sha256"]:
            return None, tr(
                f"Win64/{name} 哈希与旧目录 {candidate} 的账本不一致（可能已被修改或替换），拒绝覆盖。",
                f"Win64/{name} hash does not match ledger in {candidate} (possibly modified or replaced); refusing to overwrite.",
            )
        owned_hashes[name] = actual_sha

    host_dll_a = candidate / "host/ReShade64-game-forward.dll"
    if not _regular_file(host_dll_a) or sha256_file(host_dll_a) != owned_hashes["dxgi.dll"]:
        return None, tr(
            f"旧目录 {candidate} 缺少匹配的 host/ReShade64-game-forward.dll，无法验证 Win64/dxgi.dll 归属，拒绝覆盖。",
            f"Previous directory {candidate} lacks a matching host/ReShade64-game-forward.dll for Win64/dxgi.dll; refusing takeover.",
        )

    return VerifiedWin64Owner(
        base_root=candidate,
        ledger=ledger_a,
        owned_hashes=owned_hashes,
        raw_stub_base_path=raw_base,
    ), None


def _classify_takeover_kind(
    owner: VerifiedWin64Owner,
    current_base_root: Path,
    current_ledger: dict | None,
    package_version: str,
) -> str:
    """Return 'rollback', 'upgrade', or 'switch' for a verified side-by-side takeover."""
    old_ver = owner.ledger.get("installed_version")
    cur_res = str(current_base_root.resolve())
    owner_res = str(owner.base_root.resolve())
    if owner.ledger.get("upgraded_from") == cur_res:
        return "rollback"
    if current_ledger and current_ledger.get("migrated_to") == owner_res:
        return "rollback"
    cmp = compare_semver(old_ver, package_version)
    if cmp == -1:
        return "upgrade"
    if cmp == 1:
        return "rollback"
    if not old_ver and parse_semver(package_version) is not None:
        return "upgrade"
    return "switch"


def _classify_mount(
    win64_dir: Path,
    ledger: dict | None,
    current_base_root: Path | None = None,
    platform_kind: str | None = None,
    migrate_from: Path | None = None,
    package_version: str = "dev",
) -> tuple[str, str, dict[str, str], VerifiedWin64Owner | None]:
    active = ledger if ledger and ledger["state"] in ACTIVE_STATES else None
    if active:
        recorded = Path(active["win64_dir"]).resolve()
        if recorded != win64_dir:
            return (
                "active_elsewhere",
                tr(
                    f"账本仍绑定 {recorded}；请先卸载旧游戏目录，再安装到新目录",
                    f"Ledger is still bound to {recorded}; uninstall the previous game directory first.",
                ),
                {},
                None,
            )
        try:
            owned = _owned_bootstrap(win64_dir, active, base_root=current_base_root)
        except RuntimeError:
            # Edge case: previous owner's ledger was read-only when another version took over Win64.
            if active["state"] == "installed" and current_base_root is not None:
                owner, _ = detect_verified_win64_owner(win64_dir, current_base_root, platform_kind, migrate_from)
                if owner is not None:
                    kind = _classify_takeover_kind(owner, current_base_root, ledger, package_version)
                    old_ver_label = format_installed_version(owner.ledger)
                    if kind == "rollback":
                        msg = tr(
                            f"检测到已验证的当前版本 {old_ver_label}（{owner.base_root}）；可直接回滚至此版本 ({old_ver_label} -> {package_version})",
                            f"Verified active install {old_ver_label} ({owner.base_root}) detected; ready to roll back ({old_ver_label} -> {package_version})",
                        )
                    elif kind == "upgrade":
                        msg = tr(
                            f"检测到已验证的旧版本 {old_ver_label}（{owner.base_root}）；可直接并排升级 ({old_ver_label} -> {package_version})",
                            f"Verified previous install {old_ver_label} ({owner.base_root}) detected; ready for side-by-side upgrade ({old_ver_label} -> {package_version})",
                        )
                    else:
                        msg = tr(
                            f"检测到已验证的另一目录安装 {old_ver_label}（{owner.base_root}）；可直接并排切换至当前目录",
                            f"Verified install {old_ver_label} ({owner.base_root}) detected; ready for side-by-side switch",
                        )
                    return "upgrade_ready", msg, owner.owned_hashes, owner
            raise
        if active["state"] == "rolling_back":
            return (
                "rolling_back",
                tr(
                    "上次版本回滚未完成；请重试回滚命令/按钮完成恢复",
                    "Previous rollback was interrupted; retry Rollback to finish restoring the old folder",
                ),
                owned,
                None,
            )
        if active["state"] == "installing":
            return (
                "installing",
                tr(
                    "上次安装未完成；可在同一目录重试安装或执行卸载",
                    "Previous installation was interrupted; retry Install in the same directory or run Uninstall",
                ),
                owned,
                None,
            )
        if len(owned) != len(BOOTSTRAP_FILES):
            return (
                "drifted",
                tr(
                    "账本引导文件有缺失，其余文件归属已确认；可重新安装恢复",
                    "Some managed bootstrap files are missing (remaining files verified); re-run Install to repair",
                ),
                owned,
                None,
            )
        return "installed", tr("已正常挂载 2 文件 BasePath 外部引导桥", "Active (2-file external BasePath mount)"), owned, None

    if current_base_root is not None:
        owner, reject_reason = detect_verified_win64_owner(win64_dir, current_base_root, platform_kind, migrate_from)
        if owner is not None:
            kind = _classify_takeover_kind(owner, current_base_root, ledger, package_version)
            old_ver_label = format_installed_version(owner.ledger)
            if kind == "rollback":
                msg = tr(
                    f"检测到已验证的当前版本 {old_ver_label}（{owner.base_root}）；可直接回滚至此版本 ({old_ver_label} -> {package_version})",
                    f"Verified active install {old_ver_label} ({owner.base_root}) detected; ready to roll back ({old_ver_label} -> {package_version})",
                )
            elif kind == "upgrade":
                msg = tr(
                    f"检测到已验证的旧版本 {old_ver_label}（{owner.base_root}）；可直接并排升级 ({old_ver_label} -> {package_version})",
                    f"Verified previous install {old_ver_label} ({owner.base_root}) detected; ready for side-by-side upgrade ({old_ver_label} -> {package_version})",
                )
            else:
                msg = tr(
                    f"检测到已验证的另一目录安装 {old_ver_label}（{owner.base_root}）；可直接并排切换至当前目录",
                    f"Verified install {old_ver_label} ({owner.base_root}) detected; ready for side-by-side switch",
                )
            return "upgrade_ready", msg, owner.owned_hashes, owner
        if reject_reason is not None:
            raise RuntimeError(reject_reason)

    owned = _owned_bootstrap(win64_dir, None)
    return "not_installed", tr("Win64 未挂载引导文件", "Not mounted in Win64"), owned, None


def inspect_mount_state(
    win64_dir: Path,
    base_root: Path,
    *,
    platform_kind: str | None = None,
    migrate_from: Path | None = None,
    source_root: Path | None = None,
) -> tuple[str, str]:
    try:
        pkg_ver = read_package_provenance(source_root or base_root)["version"] or "dev"
        state, message, _, _ = _classify_mount(
            win64_dir.resolve(),
            load_install_ledger(base_root),
            current_base_root=base_root.resolve(),
            platform_kind=platform_kind,
            migrate_from=migrate_from,
            package_version=pkg_ver,
        )
        return state, message
    except (OSError, RuntimeError) as exc:
        return "conflict", str(exc)


def describe_install_action(
    win64_dir: Path,
    base_root: Path,
    *,
    source_root: Path | None = None,
    platform_kind: str | None = None,
    migrate_from: Path | None = None,
) -> dict:
    """Summarize what running Install will do: fresh install, upgrade, rollback, switch, refresh, repair, or conflict."""
    pkg_ver = read_package_provenance(source_root or base_root)["version"] or "dev"
    if pkg_ver == "dev" and source_root is not None and source_root != base_root:
        pkg_ver = read_package_provenance(base_root)["version"] or "dev"
    try:
        ledger = load_install_ledger(base_root)
        state, msg, _, owner = _classify_mount(
            win64_dir.resolve(),
            ledger,
            current_base_root=base_root.resolve(),
            platform_kind=platform_kind,
            migrate_from=migrate_from,
            package_version=pkg_ver,
        )
    except (OSError, RuntimeError) as exc:
        return {
            "action": "conflict", "mount_state": "conflict", "message": str(exc),
            "target_version": pkg_ver, "source_version": None, "owner_base_root": None,
        }
    if state == "upgrade_ready" and owner is not None:
        kind = _classify_takeover_kind(owner, base_root.resolve(), ledger, pkg_ver)
        return {
            "action": kind, "mount_state": state, "message": msg,
            "target_version": pkg_ver, "source_version": format_installed_version(owner.ledger),
            "owner_base_root": str(owner.base_root),
        }
    if state == "installed":
        return {
            "action": "refresh", "mount_state": state, "message": msg,
            "target_version": pkg_ver, "source_version": format_installed_version(ledger),
            "owner_base_root": str(base_root.resolve()),
        }
    if state in ("drifted", "installing"):
        return {
            "action": "repair", "mount_state": state, "message": msg,
            "target_version": pkg_ver, "source_version": format_installed_version(ledger),
            "owner_base_root": str(base_root.resolve()),
        }
    if state == "rolling_back":
        return {
            "action": "rolling_back", "mount_state": state, "message": msg,
            "target_version": pkg_ver, "source_version": format_installed_version(ledger),
            "owner_base_root": str(base_root.resolve()),
        }
    if state == "active_elsewhere":
        return {
            "action": "active_elsewhere", "mount_state": state, "message": msg,
            "target_version": pkg_ver, "source_version": format_installed_version(ledger),
            "owner_base_root": None,
        }
    return {
        "action": "install", "mount_state": state, "message": msg,
        "target_version": pkg_ver, "source_version": None, "owner_base_root": None,
    }


def _legacy_utf8_locale(settings: Path) -> bool:
    """Inspect literal settings without importing or executing user Python."""
    try:
        tree = ast.parse(settings.read_text(encoding="utf-8"))
    except (OSError, ValueError, SyntaxError):
        return False

    values = {}
    for node in tree.body:
        if not isinstance(node, ast.Assign):
            continue
        for target in node.targets:
            if isinstance(target, ast.Name) and target.id == "user_settings":
                try:
                    value = ast.literal_eval(node.value)
                except (ValueError, TypeError):
                    values.clear()
                else:
                    values = value if isinstance(value, dict) else {}
            elif (isinstance(target, ast.Subscript) and isinstance(target.value, ast.Name)
                  and target.value.id == "user_settings" and isinstance(target.slice, ast.Constant)
                  and isinstance(target.slice.value, str)):
                values[target.slice.value] = node.value.value if isinstance(node.value, ast.Constant) else None
    locale = next((values[k] for k in ("HOST_LC_ALL", "LC_ALL", "LC_CTYPE", "LANG") if values.get(k)), None)
    return isinstance(locale, str) and bool(re.search(r"\.utf-?8(?:@.*)?$", locale, re.IGNORECASE))


def proton_launch_options(base_root: Path, utf8_guard: bool) -> tuple[str, str | None]:
    preload = base_root.resolve() / "lib/wine-filename-utf8.so"
    warning = None
    if any(c.isspace() or c == ":" for c in str(preload)):
        warning = tr(
            "LD_PRELOAD 按空白和冒号拆分路径，引号也无法转义；如需 UTF-8 preload shim，请将工具解压并安装到不含空格和冒号的路径。",
            "LD_PRELOAD splits paths on whitespace and colons; to enable the UTF-8 preload shim, extract/install into a path without spaces or colons.",
        )
    options = ['WINEDLLOVERRIDES="dxgi=n,b"']
    if utf8_guard:
        options.append("HOST_LC_ALL=C.UTF-8")
        if not warning and preload.is_file():
            options.append(f"LD_PRELOAD={shlex.quote(str(preload))}")
    return " ".join(options + ["%command%"]), warning


def detect_wwmi_core_version(wwmi_dir: Path) -> str | None:
    core_ini = wwmi_dir / "Core/WWMI/WuWa-Model-Importer.ini"
    if not core_ini.is_file():
        return None
    try:
        text = core_ini.read_text(encoding="utf-8", errors="ignore")
    except OSError:
        return None
    m = re.search(r'data\s*=\s*"WWMI\s+([0-9]+(?:\.[0-9]+)+)', text, flags=re.IGNORECASE)
    if m:
        return m.group(1)
    m = re.search(r"global\s+\$wwmi_version\s*=\s*([0-9]+(?:\.[0-9]+)*)", text, flags=re.IGNORECASE)
    return m.group(1) if m else None


def scan_third_party_win64_addons(win64_dir: Path) -> list[str]:
    if not win64_dir.is_dir():
        return []
    own = {"wwmi_dx12.addon64", "zz_wwmi_rt_pack.addon64"}
    found: list[str] = []
    try:
        for p in sorted(win64_dir.iterdir(), key=lambda item: item.name.lower()):
            if not p.is_file():
                continue
            if p.suffix.lower() not in (".addon", ".addon64"):
                continue
            if p.name.lower() in own:
                continue
            found.append(p.name)
    except OSError:
        return []
    return found


def run_doctor(
    win64_dir: Path,
    wwmi_dir: Path,
    base_root: Path,
    platform_kind: str | None = None,
    compatdata_dir: Path | None = None,
    is_flatpak_steam: bool = False,
    source_root: Path | None = None,
    migrate_from: Path | None = None,
) -> DoctorReport:
    p_kind = platform_kind or ("windows" if sys.platform == "win32" else "linux-proton")
    findings: list[DoctorFinding] = []
    pkg_prov = read_package_provenance(source_root or base_root)
    if pkg_prov["version"] == "dev" and source_root is not None and source_root != base_root:
        base_prov = read_package_provenance(base_root)
        if base_prov["version"] != "dev" or base_prov["repository_commit"]:
            pkg_prov = base_prov
    pkg_version = pkg_prov["version"] or "dev"
    installed_version: str | None = None
    installed_commit: str | None = None
    try:
        ledger = load_install_ledger(base_root)
    except RuntimeError:
        ledger = None
    if ledger and ledger.get("state") in ACTIVE_STATES:
        installed_version = format_installed_version(ledger)
        commit_val = ledger.get("installed_commit")
        if isinstance(commit_val, str) and commit_val.strip():
            installed_commit = commit_val.strip()

    if (win64_dir / TARGET_SHIPPING_EXE).is_file():
        findings.append(DoctorFinding("ok", "exe_found", tr(
            f"已定位游戏主程序：{win64_dir / TARGET_SHIPPING_EXE}",
            f"Game executable found: {win64_dir / TARGET_SHIPPING_EXE}",
        )))
    else:
        findings.append(DoctorFinding("error", "exe_missing", tr(
            f"未找到 {TARGET_SHIPPING_EXE}：{win64_dir}",
            f"{TARGET_SHIPPING_EXE} not found in: {win64_dir}",
        )))

    mount_state, mount_msg = inspect_mount_state(
        win64_dir, base_root, platform_kind=p_kind, migrate_from=migrate_from, source_root=source_root,
    )
    if mount_state in ("installed", "drifted", "installing", "rolling_back") and installed_version:
        mount_msg = f"{mount_msg} ({tr('已装版本', 'Installed')}: {installed_version}, {tr('当前包版本', 'Package')}: {pkg_version})"
    elif ledger and ledger.get("state") == "uninstalled" and ledger.get("migrated_to") and mount_state == "upgrade_ready":
        mount_msg = f"{mount_msg} ({tr('本目录曾迁移至', 'Previously migrated to')}: {ledger['migrated_to']})"
    lvl = "ok" if mount_state in ("installed", "not_installed", "upgrade_ready") else ("warn" if mount_state == "drifted" else "error")
    findings.append(DoctorFinding(lvl, f"mount_{mount_state}", mount_msg))

    if (wwmi_dir / "d3dx.ini").is_file():
        findings.append(DoctorFinding("ok", "d3dx_ini_ok", tr(
            f"已找到 WWMI 入口配置：{wwmi_dir / 'd3dx.ini'}",
            f"WWMI entry config found: {wwmi_dir / 'd3dx.ini'}",
        )))
    else:
        findings.append(DoctorFinding("error", "d3dx_ini_missing", tr(
            f"WWMI 目录缺少 d3dx.ini：{wwmi_dir}",
            f"d3dx.ini missing in WWMI directory: {wwmi_dir}",
        )))

    core_ini = wwmi_dir / "Core/WWMI/WuWa-Model-Importer.ini"
    if core_ini.is_file():
        wwmi_ver = detect_wwmi_core_version(wwmi_dir)
        findings.append(DoctorFinding("ok", "wwmi_core_ok", tr(
            f"Core/WWMI 核心脚本齐全{f'（检测到 WWMI {wwmi_ver}）' if wwmi_ver else ''}",
            f"Core/WWMI scripts found{f' (WWMI {wwmi_ver})' if wwmi_ver else ''}",
        )))
    else:
        findings.append(
            DoctorFinding(
                "warn",
                "wwmi_core_missing",
                tr(
                    f"未检测到 {core_ini}（请确保已解压官方 WWMI-Package 核心文件）",
                    f"Missing {core_ini} (extract official WWMI-Package Core/ files here)",
                ),
            )
        )

    third_party_addons = scan_third_party_win64_addons(win64_dir)
    if third_party_addons:
        joined = ", ".join(third_party_addons)
        findings.append(
            DoctorFinding(
                "warn",
                "third_party_win64_addons",
                tr(
                    f"检测到游戏 Win64 目录存在其他 ReShade 插件：{joined}。"
                    "默认隔离模式仅加载外部 reshade-addons/；如需共存（例如 RenoDX），"
                    "可将插件放入外部 reshade-addons/，或在外部 ReShade.ini 的 [ADDON] 下添加 LoadGameDirAddons=1"
                    "（若游戏内 Mod 异常，请先临时停用第三方插件排查冲突）。",
                    f"Detected other ReShade add-on(s) in Win64: {joined}. "
                    "External mount loads <ExternalFolder>/reshade-addons/ by default; to coexist (e.g. RenoDX), "
                    "copy them into <ExternalFolder>/reshade-addons/ or set LoadGameDirAddons=1 under [ADDON] in external ReShade.ini "
                    "(disable third-party add-ons first if mods stop working in-game).",
                ),
            )
        )

    rfx_hits, non_ascii = scan_mods_health(wwmi_dir)
    if len(rfx_hits) == 1:
        findings.append(DoctorFinding("ok", "rabbitfx_unique", tr(
            f"RabbitFX 实例唯一：{rfx_hits[0]}",
            f"RabbitFX unique (1 copy): {rfx_hits[0]}",
        )))
    elif len(rfx_hits) == 0:
        findings.append(
            DoctorFinding(
                "warn",
                "rabbitfx_missing",
                tr(
                    "Mods 目录下未检测到 RabbitFX.ini（依赖 RabbitFX 槽位的皮肤可能缺失材质或发光）",
                    "RabbitFX.ini not found under Mods/ (required by some material/glow skins)",
                ),
            )
        )
    else:
        joined = ", ".join(str(p) for p in rfx_hits)
        findings.append(
            DoctorFinding(
                "error",
                "rabbitfx_duplicate",
                tr(
                    f"检测到 {len(rfx_hits)} 份重复的 RabbitFX.ini（会导致着色器冲突错乱，请只保留 1 份）：{joined}",
                    f"Detected {len(rfx_hits)} duplicate copies of RabbitFX.ini (keep only ONE copy): {joined}",
                ),
            )
        )

    if p_kind == "linux-proton":
        if is_flatpak_steam:
            fp_root = Path.home().resolve() / ".var/app/com.valvesoftware.Steam"
            compat_res = compatdata_dir.resolve() if compatdata_dir is not None else None
            base_res = base_root.resolve()
            if not (base_res.is_relative_to(fp_root) or (compat_res is not None and base_res.is_relative_to(compat_res))):
                findings.append(DoctorFinding(
                    "warn", "flatpak_base_sandbox",
                    tr(
                        f"Flatpak Steam 默认沙箱可能无法读取外部挂载根目录（{base_res}）；"
                        f"请迁移至 Flatpak 数据目录或执行：flatpak override --user --filesystem={shlex.quote(str(base_res))} com.valvesoftware.Steam",
                        f"Flatpak Steam's sandbox may block access to external base root ({base_res}); "
                        f"move it inside Flatpak's data dir or run: flatpak override --user --filesystem={shlex.quote(str(base_res))} com.valvesoftware.Steam",
                    ),
                ))
            wwmi_res = wwmi_dir.resolve()
            in_sandbox = wwmi_res.is_relative_to(fp_root) or (
                compat_res is not None and wwmi_res.is_relative_to(compat_res)
            )
            if not in_sandbox:
                findings.append(DoctorFinding(
                    "warn", "flatpak_wwmi_sandbox",
                    tr(
                        f"Flatpak Steam 默认沙箱可能无法读取宿主机 WWMI 目录（{wwmi_res}）；"
                        f"请迁移至 Flatpak 数据目录或执行：flatpak override --user --filesystem={shlex.quote(str(wwmi_res))} com.valvesoftware.Steam",
                        f"Flatpak Steam's sandbox may block access to host WWMI directory ({wwmi_res}); "
                        f"move it inside Flatpak's data dir or run: flatpak override --user --filesystem={shlex.quote(str(wwmi_res))} com.valvesoftware.Steam",
                    ),
                ))
        options, warning = proton_launch_options(base_root, utf8_guard=True)
        if warning:
            findings.append(DoctorFinding("warn", "linux_preload_path", warning))
        if non_ascii:
            legacy = compatdata_dir / "user_settings.py" if compatdata_dir else None
            if legacy and _legacy_utf8_locale(legacy):
                detail = tr(
                    "旧 compatdata/user_settings.py 中检测到 UTF-8 locale，但现代 Proton 不从此处读取。",
                    "Legacy compatdata/user_settings.py specifies a UTF-8 locale, which modern Proton ignores.",
                )
            else:
                detail = tr(
                    f"检测到 {len(non_ascii)} 个非 ASCII Mod 目录（如 {non_ascii[0]}）。",
                    f"Detected {len(non_ascii)} non-ASCII mod path(s) (e.g. {non_ascii[0]}).",
                )
            findings.append(DoctorFinding(
                "warn", "linux_utf8_notice",
                tr(
                    f"{detail} 如需 UTF-8 护航，请在 Steam 本游戏的启动选项中填写：{options}",
                    f"{detail} For UTF-8 filename safety, set Steam Launch Options to: {options}",
                ),
            ))

    return DoctorReport(
        win64_dir=str(win64_dir),
        wwmi_dir=str(wwmi_dir),
        base_root=str(base_root),
        mount_state=mount_state,
        rabbitfx_count=len(rfx_hits),
        non_ascii_mods=non_ascii,
        findings=findings,
        package_version=pkg_version,
        installed_version=installed_version,
        installed_commit=installed_commit,
    )


def _validated_payload(mapping: dict[str, Path]) -> dict[str, Path]:
    for rel in REQUIRED_PAYLOAD:
        src = mapping.get(rel)
        if not src or not src.is_file() or not src.stat().st_size:
            raise FileNotFoundError(f"缺少或为空的运行时组件：{rel}，请重新构建或完整解压")
    return mapping


def resolve_payload_sources(source_root: Path) -> dict[str, Path]:
    """Locate runtime binaries from either a repo checkout (payload/artifacts/stage5) or a pre-packed release dir."""
    s5 = source_root / "payload/artifacts/stage5"
    if (s5 / "ReShade64-game-forward.dll").is_file():
        mapping: dict[str, Path] = {
            "host/ReShade64-game-forward.dll": s5 / "ReShade64-game-forward.dll",
            "reshade-addons/wwmi_dx12.addon64": s5 / "wwmi_dx12.addon64",
            "reshade-addons/zz_wwmi_rt_pack.addon64": s5 / "zz_wwmi_rt_pack.addon64",
        }
        for sub in ("wwmi_dx12", "wwmi_dx12/dxc", "rt-pack-data"):
            src_dir = s5 / sub
            if src_dir.is_dir():
                for f in sorted(src_dir.iterdir()):
                    if f.is_file():
                        mapping[f"reshade-addons/{sub}/{f.name}"] = f
        utf8_so = source_root / "lib/wine-filename-utf8.so"
        if utf8_so.is_file():
            mapping["lib/wine-filename-utf8.so"] = utf8_so
        seed_dir = source_root / "payload/seed"
        if seed_dir.is_dir():
            for f in sorted(seed_dir.iterdir()):
                if f.is_file() and f.suffix in (".texlook", ".lodcache"):
                    mapping[f"seed/{f.name}"] = f
        return _validated_payload(mapping)

    # Pre-packed portable release layout (source_root == base_root or unpacked archive)
    host_dll = source_root / "host/ReShade64-game-forward.dll"
    if host_dll.is_file() and (source_root / "reshade-addons/wwmi_dx12.addon64").is_file():
        mapping = {
            "host/ReShade64-game-forward.dll": host_dll,
            "reshade-addons/wwmi_dx12.addon64": source_root / "reshade-addons/wwmi_dx12.addon64",
            "reshade-addons/zz_wwmi_rt_pack.addon64": source_root / "reshade-addons/zz_wwmi_rt_pack.addon64",
        }
        for sub in ("wwmi_dx12", "wwmi_dx12/dxc", "rt-pack-data"):
            src_dir = source_root / "reshade-addons" / sub
            if src_dir.is_dir():
                for f in sorted(src_dir.iterdir()):
                    if f.is_file():
                        mapping[f"reshade-addons/{sub}/{f.name}"] = f
        utf8_so = source_root / "lib/wine-filename-utf8.so"
        if utf8_so.is_file():
            mapping["lib/wine-filename-utf8.so"] = utf8_so
        seed_dir = source_root / "seed"
        if seed_dir.is_dir():
            for f in sorted(seed_dir.iterdir()):
                if f.is_file() and f.suffix in (".texlook", ".lodcache"):
                    mapping[f"seed/{f.name}"] = f
        return _validated_payload(mapping)

    raise FileNotFoundError(
        f"未在 {source_root} 中找到编译产物（请先运行 python3 scripts/build.py 或在完整发行包目录中运行）"
    )


def default_staging_root(base_root: Path) -> Path:
    if sys.platform == "win32":
        return base_root / "runtime/uninstall-staging"
    return Path.home() / ".pi/deletion-staging"


def _atomic_write(path: Path, data: bytes) -> None:
    temp = path.with_name(f".{path.name}.{uuid.uuid4().hex}.tmp")
    _write_new_file(temp, data)
    os.replace(temp, path)


def _write_new_file(path: Path, data: bytes) -> None:
    with path.open("xb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())


def _write_json(path: Path, obj: dict) -> None:
    _atomic_write(path, (json.dumps(obj, indent=2, ensure_ascii=False) + "\n").encode("utf-8"))


def stage_files_for_removal(paths: Sequence[Path], staging_root: Path, label: str) -> Path:
    """Record every planned move before moving the first file."""
    stamp = _dt.datetime.now().strftime("%Y%m%d-%H%M%S")
    batch_dir = staging_root.resolve() / f"batch-{stamp}-{label}-{uuid.uuid4().hex[:8]}"
    batch_dir.mkdir(mode=0o700, parents=True)
    entries = [
        {"original_path": str(p), "staged_path": str(batch_dir / f"{idx:03d}_{p.name}")}
        for idx, p in enumerate(paths)
    ]
    manifest_path = batch_dir / "manifest.json"
    _write_json(manifest_path, {
        "created_at": _dt.datetime.now(_dt.timezone.utc).isoformat(),
        "label": label,
        "entries": entries,
    })
    for entry in entries:
        src, dst = entry["original_path"], entry["staged_path"]
        try:
            if shutil.move(src, dst) != dst or os.path.lexists(src) or not os.path.lexists(dst):
                raise OSError("移动后的文件状态不符")
        except OSError as exc:
            raise RuntimeError(f"暂存未完成：{src}；已保留恢复清单 {manifest_path}：{exc}") from exc
    return batch_dir


def _linux_fixed_lock_root() -> Path:
    """Environment-independent per-user lock root on POSIX (ignores XDG_CACHE_HOME / TMPDIR)."""
    try:
        import pwd
        pw_home = Path(pwd.getpwuid(os.getuid()).pw_dir)
        root = pw_home / ".cache/wwmi-dx12-rt/locks"
        root.mkdir(parents=True, exist_ok=True)
        return root
    except (ImportError, KeyError, OSError):
        fallback = Path("/tmp") / f"wwmi-dx12-rt-locks-{os.getuid()}"
        fallback.mkdir(parents=True, exist_ok=True)
        return fallback


def _fold_windows_lock_path(text: str) -> str:
    """Deterministic Win32 path folding identical to C++ FoldWindowsLockPath (LCMapStringEx INVARIANT UPPERCASE)."""
    s = text.replace("/", "\\")
    while len(s) > 3 and s.endswith("\\"):
        s = s[:-1]
    if sys.platform == "win32":
        import ctypes
        kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel32.LCMapStringEx.argtypes = [
            ctypes.c_wchar_p, ctypes.c_uint32, ctypes.c_wchar_p, ctypes.c_int,
            ctypes.c_wchar_p, ctypes.c_int, ctypes.c_void_p, ctypes.c_void_p, ctypes.c_ssize_t,
        ]
        kernel32.LCMapStringEx.restype = ctypes.c_int
        in_len = len(s.encode("utf-16-le")) // 2
        if in_len == 0:
            return ""
        buf_in = ctypes.create_unicode_buffer(s)
        needed = int(kernel32.LCMapStringEx("", 0x00000200, buf_in, in_len, None, 0, None, None, 0))
        if needed > 0:
            buf_out = ctypes.create_unicode_buffer(needed + 1)
            got = int(kernel32.LCMapStringEx("", 0x00000200, buf_in, in_len, buf_out, needed, None, None, 0))
            if got == needed:
                return ctypes.wstring_at(buf_out, got)
    return "".join(chr(ord(c) - 32) if "a" <= c <= "z" else c for c in s)


def _win64_lock_key(win64_dir: Path) -> str:
    canonical = win64_dir.resolve()
    key_src = _fold_windows_lock_path(str(canonical)) if sys.platform == "win32" else str(canonical)
    return hashlib.sha256(key_src.encode("utf-8")).hexdigest()[:24]


def _win64_lock_path(win64_dir: Path) -> Path:
    digest = _win64_lock_key(win64_dir)
    if sys.platform == "win32":
        root = Path(os.environ.get("LOCALAPPDATA") or tempfile.gettempdir()) / "WWMI-DX12-RT/locks"
        return root / f"win64-{digest}.lock"
    return _linux_fixed_lock_root() / f"win64-{digest}.lock"


@contextlib.contextmanager
def _windows_named_mutex(mutex_name: str, busy_msg: str):
    import ctypes
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.CreateMutexW.argtypes = [ctypes.c_void_p, ctypes.c_int, ctypes.c_wchar_p]
    kernel32.CreateMutexW.restype = ctypes.c_void_p
    kernel32.WaitForSingleObject.argtypes = [ctypes.c_void_p, ctypes.c_uint32]
    kernel32.WaitForSingleObject.restype = ctypes.c_uint32
    kernel32.ReleaseMutex.argtypes = [ctypes.c_void_p]
    kernel32.ReleaseMutex.restype = ctypes.c_int
    kernel32.CloseHandle.argtypes = [ctypes.c_void_p]
    kernel32.CloseHandle.restype = ctypes.c_int

    handle = kernel32.CreateMutexW(None, 0, mutex_name)
    if not handle:
        raise RuntimeError(busy_msg)
    owns = False
    try:
        wait = kernel32.WaitForSingleObject(handle, 0)
        if wait not in (0x00000000, 0x00000080):  # WAIT_OBJECT_0 / WAIT_ABANDONED
            raise RuntimeError(busy_msg)
        owns = True
        yield
    finally:
        if owns:
            kernel32.ReleaseMutex(handle)
        kernel32.CloseHandle(handle)


@contextlib.contextmanager
def _win64_lock(win64_dir: Path):
    """Canonical-Win64 exclusive lock shared across all entry points (ignores XDG_CACHE_HOME; uses Win32 named mutex on Windows)."""
    if sys.platform == "win32":
        name = f"Local\\WWMI-DX12-RT-Win64-{_win64_lock_key(win64_dir)}"
        msg = tr(
            f"无法锁定目标 Win64 目录，可能有另一安装/升级/卸载正在运行：{win64_dir}",
            f"Could not lock target Win64 directory; another install/upgrade/uninstall may be running: {win64_dir}",
        )
        with _windows_named_mutex(name, msg):
            yield
        return
    path = _win64_lock_path(win64_dir)
    path.parent.mkdir(parents=True, exist_ok=True)
    _regular_file(path)
    with path.open("a+b") as lock:
        try:
            import fcntl
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            raise RuntimeError(tr(
                f"无法锁定目标 Win64 目录，可能有另一安装/升级/卸载正在运行：{path}",
                f"Could not lock target Win64 directory; another install/upgrade/uninstall may be running: {path}",
            )) from exc
        yield  # Closing the file releases the OS lock, including after a crash.


@contextlib.contextmanager
def _base_root_locks(*roots: Path | None):
    """Acquire exclusive per-base_root ledger locks in deterministic canonical order (prevents concurrent multi-Win64 binding)."""
    unique: list[Path] = []
    seen: set[str] = set()
    for r in roots:
        if r is None:
            continue
        res = r.resolve()
        k = _fold_windows_lock_path(str(res)) if sys.platform == "win32" else str(res)
        if k not in seen:
            seen.add(k)
            unique.append(res)
    unique.sort(key=lambda p: _fold_windows_lock_path(str(p)) if sys.platform == "win32" else str(p))
    with contextlib.ExitStack() as stack:
        for base in unique:
            if sys.platform == "win32":
                digest = hashlib.sha256(_fold_windows_lock_path(str(base)).encode("utf-8")).hexdigest()[:24]
                msg = tr(
                    f"外部根目录账本正被另一进程锁定：{base}",
                    f"External base root ledger is locked by another process: {base}",
                )
                stack.enter_context(_windows_named_mutex(f"Local\\WWMI-DX12-RT-Base-{digest}", msg))
            else:
                lock_path = base / "runtime/.install.lock"
                lock_path.parent.mkdir(parents=True, exist_ok=True)
                _regular_file(lock_path)
                f = stack.enter_context(lock_path.open("a+b"))
                try:
                    import fcntl
                    fcntl.flock(f, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except OSError as exc:
                    raise RuntimeError(tr(
                        f"外部根目录账本正被另一进程锁定：{lock_path}",
                        f"External base root ledger is locked by another process: {lock_path}",
                    )) from exc
        yield


def build_external_reshade_ini() -> str:
    return (
        "[INSTALL]\n"
        "Logging=0\n\n"
        "[ADDON]\n"
        "AddonPath=reshade-addons\n\n"
        "[OVERLAY]\n"
        "TutorialProgress=4\n"
    )


def _parse_ini_section_name(line: str) -> str | None:
    """Match ReShade's exact case-sensitive section parsing (line[0] == '[')."""
    stripped = line.strip()
    if not stripped.startswith("["):
        return None
    close = stripped.find("]")
    raw = stripped[:close] if close != -1 else stripped
    return raw.strip(" \t[]")


def _split_addon_path_tokens(raw: str) -> list[str]:
    """Split ReShade's comma-separated list (decoding ',,' as literal ',') and ';' sub-tokens."""
    comma_parts: list[str] = []
    buf: list[str] = []
    i = 0
    n = len(raw)
    while i < n:
        ch = raw[i]
        if ch == ",":
            if i + 1 < n and raw[i + 1] == ",":
                buf.append(",")
                i += 2
                continue
            comma_parts.append("".join(buf))
            buf.clear()
            i += 1
            continue
        buf.append(ch)
        i += 1
    comma_parts.append("".join(buf))

    out: list[str] = []
    for part in comma_parts:
        for tok in part.split(";"):
            cleaned = tok.strip()
            if cleaned:
                out.append(cleaned)
    return out


def merge_external_reshade_ini(existing_text: str | None) -> str:
    """Preserve user ReShade.ini customizations across upgrades while ensuring reshade-addons is searched first."""
    if not existing_text:
        return build_external_reshade_ini()
    if existing_text.startswith("\ufeff"):
        existing_text = existing_text[1:]
    if not existing_text.strip():
        return build_external_reshade_ini()
    eol = "\r\n" if "\r\n" in existing_text else "\n"
    lines = existing_text.splitlines()
    in_addon = False
    addon_header_idx = -1
    addon_path_indices: list[int] = []
    for i, line in enumerate(lines):
        stripped = line.strip()
        if not stripped or stripped.startswith((";", "#", "/")):
            continue
        sec = _parse_ini_section_name(line)
        if sec is not None:
            in_addon = sec == "ADDON"
            if in_addon and addon_header_idx < 0:
                addon_header_idx = i
            continue
        if in_addon and "=" in line:
            key, _, _val = line.partition("=")
            if key.strip() == "AddonPath":
                addon_path_indices.append(i)
    if addon_path_indices:
        extras: list[str] = []
        seen_extras: set[str] = set()
        for idx in addon_path_indices:
            _, _, val = lines[idx].partition("=")
            for tok in _split_addon_path_tokens(val):
                norm = tok.replace("\\", "/").rstrip("/").lower()
                if norm == "reshade-addons":
                    continue
                if norm not in seen_extras:
                    seen_extras.add(norm)
                    extras.append(tok.replace(",", ",,"))
        first_idx = addon_path_indices[0]
        merged_val = "reshade-addons"
        for extra in extras:
            merged_val += (", " if extra.startswith(",") else ",") + extra
        lines[first_idx] = f"AddonPath={merged_val}"
        for dup_idx in reversed(addon_path_indices[1:]):
            lines.pop(dup_idx)
    elif addon_header_idx >= 0:
        lines.insert(addon_header_idx + 1, "AddonPath=reshade-addons")
    else:
        if lines and lines[-1].strip():
            lines.append("")
        lines.extend(["[ADDON]", "AddonPath=reshade-addons"])
    return eol.join(lines) + eol


def build_win64_stub_reshade_ini(engine_base_path: str) -> str:
    return f"[INSTALL]\nBasePath={engine_base_path}\n"


def build_wwmi_dx12_ini(
    engine_wwmi_dir: str,
    engine_runtime_dir: str,
    skin_mode: str = "base",
    max_logs: int = 10,
    debug: int = 0,
) -> str:
    return (
        "; Managed by WWMI-DX12-RT Setup. Safe to edit manually.\n"
        "[wwmi-dx12]\n"
        f"wwmi_dir={engine_wwmi_dir}\n"
        f"skin_mode={skin_mode}\n"
        f"runtime_dir={engine_runtime_dir}\n"
        f"max_logs={max_logs}\n"
        "tex_struct_fallback=1\n"
        f"debug={debug}\n"
        "log_level=1\n"
    )


def encode_wwmi_dx12_ini(text: str) -> bytes:
    normalized = text.replace("\r\n", "\n").replace("\r", "\n")
    return b"\xff\xfe" + normalized.replace("\n", "\r\n").encode("utf-16-le")


def decode_ini_bytes(data: bytes) -> str:
    if data.startswith(b"\xff\xfe"):
        return data[2:].decode("utf-16-le", errors="replace")
    if data.startswith(b"\xfe\xff"):
        return data[2:].decode("utf-16-be", errors="replace")
    if data.startswith(b"\xef\xbb\xbf"):
        return data[3:].decode("utf-8", errors="replace")
    if b"\x00" in data[:64]:
        return data.decode("utf-16-le", errors="replace")
    return data.decode("utf-8", errors="replace")


def parse_wwmi_ini_values(text: str) -> dict[str, str]:
    values: dict[str, str] = {}
    in_section = False
    for line in text.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith((";", "#", "/")):
            continue
        sec = _parse_ini_section_name(line)
        if sec is not None:
            in_section = sec.lower() == "wwmi-dx12"
            continue
        if in_section and "=" in line:
            key, _, val = line.partition("=")
            values[key.strip().lower()] = val.strip()
    return values


def _path_is_within_engine_base(raw_val: str, engine_base: str | None) -> bool:
    if not raw_val or not engine_base:
        return False
    norm_val = raw_val.strip().strip('"').replace("/", "\\").rstrip("\\").lower()
    norm_base = engine_base.strip().strip('"').replace("/", "\\").rstrip("\\").lower()
    return norm_val == norm_base or norm_val.startswith(norm_base + "\\")


def merge_wwmi_dx12_ini(
    existing_bytes: bytes | None,
    engine_wwmi_dir: str,
    engine_runtime_dir: str,
    skin_mode: str = "base",
    old_engine_base: str | None = None,
    force_skin_mode: bool = False,
) -> bytes:
    """Preserve user keys/comments in wwmi_dx12.ini (UTF-16LE BOM + CRLF), updating only migrated paths."""
    if not existing_bytes:
        return encode_wwmi_dx12_ini(build_wwmi_dx12_ini(engine_wwmi_dir, engine_runtime_dir, skin_mode=skin_mode))
    text = decode_ini_bytes(existing_bytes)
    if text.startswith("\ufeff"):
        text = text[1:]
    if not text.strip():
        return encode_wwmi_dx12_ini(build_wwmi_dx12_ini(engine_wwmi_dir, engine_runtime_dir, skin_mode=skin_mode))

    lines = text.splitlines()
    in_wwmi = False
    wwmi_header_idx = -1
    wwmi_section_end = -1
    seen_keys: set[str] = set()

    for i, line in enumerate(lines):
        stripped = line.strip()
        if not stripped or stripped.startswith((";", "#", "/")):
            continue
        sec = _parse_ini_section_name(line)
        if sec is not None:
            if in_wwmi and wwmi_section_end < 0:
                wwmi_section_end = i
            in_wwmi = sec.lower() == "wwmi-dx12"
            if in_wwmi and wwmi_header_idx < 0:
                wwmi_header_idx = i
            continue
        if in_wwmi and "=" in line:
            key, _, val = line.partition("=")
            k_clean = key.strip()
            k_low = k_clean.lower()
            seen_keys.add(k_low)
            if k_low == "wwmi_dir":
                lines[i] = f"wwmi_dir={engine_wwmi_dir}"
            elif k_low == "runtime_dir":
                cur_val = val.strip()
                if not cur_val or (old_engine_base is not None and _path_is_within_engine_base(cur_val, old_engine_base)):
                    lines[i] = f"runtime_dir={engine_runtime_dir}"
            elif k_low == "skin_mode" and force_skin_mode:
                lines[i] = f"skin_mode={skin_mode}"

    missing_entries: list[str] = []
    if "wwmi_dir" not in seen_keys:
        missing_entries.append(f"wwmi_dir={engine_wwmi_dir}")
    if "skin_mode" not in seen_keys:
        missing_entries.append(f"skin_mode={skin_mode}")
    if "runtime_dir" not in seen_keys:
        missing_entries.append(f"runtime_dir={engine_runtime_dir}")

    if wwmi_header_idx >= 0:
        if missing_entries:
            insert_at = wwmi_section_end if wwmi_section_end >= 0 else len(lines)
            for offset, entry in enumerate(missing_entries):
                lines.insert(insert_at + offset, entry)
    else:
        if lines and lines[-1].strip():
            lines.append("")
        lines.extend([
            "[wwmi-dx12]",
            f"wwmi_dir={engine_wwmi_dir}",
            f"skin_mode={skin_mode}",
            f"runtime_dir={engine_runtime_dir}",
            "max_logs=10",
            "tex_struct_fallback=1",
            "debug=0",
            "log_level=1",
        ])
    return encode_wwmi_dx12_ini("\n".join(lines) + "\n")


_RESHADE_PATH_KEYS = frozenset({
    "presetpath",
    "effectsearchpaths",
    "texturesearchpaths",
    "intermediatecachepath",
    "screenshotpath",
    "extraaddonpaths",
    "addonpath",
})


def _is_relative_reshade_path(token: str) -> bool:
    t = token.strip().strip('"')
    if not t:
        return False
    norm = t.replace("\\", "/").rstrip("/").lower()
    if norm in ("reshade-addons", "."):
        return False
    if t.startswith(("/", "\\", "%")):
        return False
    if len(t) >= 2 and t[0].isalpha() and t[1] == ":":
        return False
    if t.isdigit():
        return False
    return True


def scan_reshade_ini_migration_warnings(ini_text: str, old_base_root: Path, platform_kind: str) -> list[str]:
    """Warn on absolute paths pointing to old_base_root or relative paths in ReShade.ini (never blind-replace)."""
    warnings: list[str] = []
    old_host_norm = str(old_base_root.resolve()).replace("\\", "/").rstrip("/").lower()
    old_eng_norm = format_engine_path(old_base_root.resolve(), platform_kind).replace("\\", "/").rstrip("/").lower()
    cur_section = ""
    seen: set[str] = set()

    for line in ini_text.splitlines():
        stripped = line.strip()
        if not stripped or stripped.startswith((";", "#", "/")):
            continue
        sec = _parse_ini_section_name(line)
        if sec is not None:
            cur_section = sec
            continue
        if "=" not in line:
            continue
        key, _, val = line.partition("=")
        k_clean = key.strip()
        v_clean = val.strip()
        if not v_clean:
            continue
        v_norm = v_clean.replace("\\", "/").lower()
        refs_old = (old_host_norm and old_host_norm in v_norm) or (old_eng_norm and old_eng_norm in v_norm)
        if refs_old:
            tag = f"abs:{cur_section}:{k_clean}"
            if tag not in seen:
                seen.add(tag)
                warnings.append(tr(
                    f"外部 ReShade.ini 中 [{cur_section}] {k_clean} 仍包含旧目录路径（{old_base_root}），未自动替换，请按需检查调整。",
                    f"External ReShade.ini [{cur_section}] {k_clean} still references old folder ({old_base_root}); left unmodified — review if needed.",
                ))
            continue
        if k_clean.lower() in _RESHADE_PATH_KEYS:
            rel_tokens = [tok for tok in _split_addon_path_tokens(v_clean) if _is_relative_reshade_path(tok)]
            if rel_tokens:
                tag = f"rel:{cur_section}:{k_clean}"
                if tag not in seen:
                    seen.add(tag)
                    joined = ", ".join(rel_tokens[:3])
                    warnings.append(tr(
                        f"外部 ReShade.ini 中 [{cur_section}] {k_clean} 包含相对路径（{joined}）；若对应预设/着色器仍在旧目录，请手动复制到新目录。",
                        f"External ReShade.ini [{cur_section}] {k_clean} uses relative path(s) ({joined}); copy those files from the old folder if needed.",
                    ))
    return warnings


def _wwmi_dir_has_content(wwmi_dir: Path) -> bool:
    if not (wwmi_dir / "d3dx.ini").is_file():
        return False
    for sub in ("Core", "Mods"):
        d = wwmi_dir / sub
        if d.is_dir():
            try:
                if any(d.iterdir()):
                    return True
            except OSError:
                continue
    return False


def resolve_active_wwmi_dir_from_base(base_root: Path, ledger: dict | None, platform_kind: str) -> Path | None:
    """Resolve effective WWMI directory for base_root: active wwmi_dx12.ini takes precedence over ledger."""
    ini_path = base_root / "wwmi_dx12.ini"
    try:
        if _regular_file(ini_path):
            vals = parse_wwmi_ini_values(decode_ini_bytes(ini_path.read_bytes()))
            mapped = resolve_engine_path_to_host(vals.get("wwmi_dir"), platform_kind)
            if mapped and mapped.is_dir():
                return mapped
    except (OSError, RuntimeError):
        pass
    if ledger:
        cand_raw = ledger.get("wwmi_dir")
        if isinstance(cand_raw, str) and cand_raw.strip():
            p = Path(cand_raw)
            if p.is_absolute() and p.is_dir():
                return p.resolve()
    return None


def resolve_owner_wwmi_dir(owner: VerifiedWin64Owner, platform_kind: str) -> Path | None:
    """Resolve existing WWMI directory from previous owner's active wwmi_dx12.ini first, then ledger."""
    return resolve_active_wwmi_dir_from_base(owner.base_root, owner.ledger, platform_kind)


def resolve_old_runtime_dir(old_base_root: Path, platform_kind: str) -> Path:
    ini_path = old_base_root / "wwmi_dx12.ini"
    try:
        if _regular_file(ini_path):
            vals = parse_wwmi_ini_values(decode_ini_bytes(ini_path.read_bytes()))
            mapped = resolve_engine_path_to_host(vals.get("runtime_dir"), platform_kind)
            if mapped and mapped.is_dir():
                return mapped
    except (OSError, RuntimeError):
        pass
    return (old_base_root / "runtime").resolve()


def migrate_cache_files(old_runtime_dir: Path, new_runtime_dir: Path) -> list[str]:
    """Copy compatible cache files from old runtime/cache to new runtime/cache if not already newer in destination."""
    old_cache = old_runtime_dir / "cache"
    new_cache = new_runtime_dir / "cache"
    if not old_cache.is_dir() or old_cache.resolve() == new_cache.resolve():
        return []
    new_cache.mkdir(parents=True, exist_ok=True)
    copied: list[str] = []
    for entry in sorted(old_cache.iterdir(), key=lambda p: p.name.lower()):
        if entry.suffix.lower() not in COMPATIBLE_CACHE_SUFFIXES:
            continue
        try:
            if not _regular_file(entry):
                continue
        except RuntimeError:
            continue
        dst = new_cache / entry.name
        src_stat = entry.stat()
        if dst.exists():
            try:
                if _regular_file(dst) and dst.stat().st_mtime >= src_stat.st_mtime:
                    continue
            except (OSError, RuntimeError):
                continue
        _atomic_write(dst, entry.read_bytes())
        try:
            os.utime(dst, (src_stat.st_atime, src_stat.st_mtime))
        except OSError:
            pass
        copied.append(entry.name)
    return copied


def migrate_third_party_addons(old_base_root: Path, new_base_root: Path) -> tuple[list[str], list[str]]:
    """Copy user-added *.addon64 from old reshade-addons/ excluding WWMI-owned payload files."""
    old_dir = old_base_root / "reshade-addons"
    new_dir = new_base_root / "reshade-addons"
    copied: list[str] = []
    warnings: list[str] = []
    if not old_dir.is_dir() or old_dir.resolve() == new_dir.resolve():
        return copied, warnings
    new_dir.mkdir(parents=True, exist_ok=True)
    existing_lower = {p.name.lower(): p.name for p in new_dir.iterdir() if p.is_file()}
    for entry in sorted(old_dir.iterdir(), key=lambda p: p.name.lower()):
        if entry.suffix.lower() != ".addon64":
            continue
        low = entry.name.lower()
        if low in WWMI_OWNED_ADDON_FILES:
            continue
        try:
            if not _regular_file(entry):
                continue
        except RuntimeError:
            continue
        if low in existing_lower:
            warnings.append(tr(
                f"第三方插件文件名冲突（{entry.name}）：已保留新目录中的现有文件，未从旧目录覆盖。",
                f"Third-party add-on filename conflict ({entry.name}): kept existing file in new folder instead of overwriting from old folder.",
            ))
            continue
        dst = new_dir / entry.name
        _atomic_write(dst, entry.read_bytes())
        existing_lower[low] = entry.name
        copied.append(entry.name)
    return copied, warnings


def _check_flatpak_visibility_warnings(
    base_root: Path,
    wwmi_dir: Path,
    compatdata_dir: Path | None,
    is_flatpak_steam: bool,
) -> list[str]:
    if not is_flatpak_steam:
        return []
    fp_root = Path.home().resolve() / ".var/app/com.valvesoftware.Steam"
    compat_res = compatdata_dir.resolve() if compatdata_dir is not None else None
    warnings: list[str] = []
    for label_zh, label_en, target in (
        ("外部挂载根目录", "external base root", base_root.resolve()),
        ("WWMI Mod 目录", "WWMI directory", wwmi_dir.resolve()),
    ):
        in_sandbox = target.is_relative_to(fp_root) or (compat_res is not None and target.is_relative_to(compat_res))
        if not in_sandbox:
            warnings.append(tr(
                f"Flatpak Steam 默认沙箱可能无法读取{label_zh}（{target}）；请执行：flatpak override --user --filesystem={shlex.quote(str(target))} com.valvesoftware.Steam",
                f"Flatpak Steam sandbox may block access to {label_en} ({target}); run: flatpak override --user --filesystem={shlex.quote(str(target))} com.valvesoftware.Steam",
            ))
    return warnings


def _write_recovery_file(path: Path, data: bytes) -> None:
    """Exclusive write+fsync used for rollback restoration without depending on test mocks of _write_new_file."""
    with path.open("xb") as f:
        f.write(data)
        f.flush()
        os.fsync(f.fileno())


def _create_recovery_backup(
    win64_dir: Path,
    owned_names: Sequence[str],
    base_root: Path,
    pending_id: str,
) -> tuple[Path, dict[str, Path]]:
    recovery_dir = base_root / "runtime/recovery" / pending_id
    recovery_dir.mkdir(parents=True, exist_ok=True)
    backups: dict[str, Path] = {}
    entries: list[dict[str, str]] = []
    for name in owned_names:
        src = win64_dir / name
        dst = recovery_dir / name
        data = src.read_bytes()
        _write_recovery_file(dst, data)
        backups[name] = dst
        entries.append({
            "relative": name,
            "original_path": str(src),
            "backup_path": str(dst),
            "sha256": hashlib.sha256(data).hexdigest(),
        })
    _write_json(recovery_dir / "manifest.json", {
        "created_at": _dt.datetime.now(_dt.timezone.utc).isoformat(),
        "pending_id": pending_id,
        "win64_dir": str(win64_dir),
        "entries": entries,
    })
    return recovery_dir, backups


def _check_install_target(win64_dir: Path, base_root: Path, wwmi_dir: Path, check_processes: bool) -> None:
    if not (win64_dir / TARGET_SHIPPING_EXE).is_file():
        raise ValueError(f"无效的 Win64 游戏目录（缺少 {TARGET_SHIPPING_EXE}）：{win64_dir}")
    if any(c == "," or ord(c) < 32 or 127 <= ord(c) <= 159 for c in str(base_root)):
        raise ValueError("BasePath 不得包含英文逗号或控制字符（ReShade INI 会拆分或截断路径）")
    game_root = win64_dir
    if tuple(p.lower() for p in win64_dir.parts[-3:]) == ("client", "binaries", "win64"):
        game_root = win64_dir.parents[2]
    if base_root.is_relative_to(game_root):
        raise ValueError(f"BasePath 必须位于游戏目录之外：{game_root}")
    # In a portable release WWMI can be a child of BasePath, but never the other way round.
    if base_root.is_relative_to(wwmi_dir):
        raise ValueError(f"BasePath 必须位于关联 WWMI 目录之外：{wwmi_dir}")
    for relative in ("runtime", "runtime/logs", "runtime/cache/shaders", "runtime/seed", "runtime/recovery",
                     "host", "reshade-addons/wwmi_dx12/dxc", "reshade-addons/rt-pack-data", "seed", "lib"):
        directory = base_root / relative
        actual = directory.resolve()
        if not actual.is_relative_to(base_root) or actual.is_relative_to(wwmi_dir):
            raise ValueError(f"拒绝通过外部运行时目录写入其他目录：{directory}")
    if check_processes and (running := running_watched_processes()):
        raise RuntimeError(f"检测到游戏正在运行（{', '.join(running)}），请先正常退出游戏")


def _pending_files(win64_dir: Path, ledger: dict | None) -> list[Path]:
    if not ledger or ledger["state"] not in PENDING_STATES:
        return []
    paths = [win64_dir / f".wwmi-{ledger['pending_id']}-{name}.tmp" for name in BOOTSTRAP_FILES]
    return [path for path in paths if _regular_file(path)]


def _select_retained_wwmi(
    requested_wwmi: Path,
    base_root: Path,
    owner: VerifiedWin64Owner | None,
    platform_kind: str,
) -> tuple[Path, bool]:
    """Retain previous owner's populated WWMI directory by default instead of replacing with an empty B/WWMI."""
    if owner is None:
        return requested_wwmi, False
    old_wwmi = resolve_owner_wwmi_dir(owner, platform_kind)
    if old_wwmi is None or not _wwmi_dir_has_content(old_wwmi):
        return requested_wwmi, False
    if base_root.is_relative_to(old_wwmi):
        return requested_wwmi, False
    if requested_wwmi == old_wwmi:
        return old_wwmi, False
    if not (requested_wwmi / "d3dx.ini").is_file() or (
        requested_wwmi == (base_root / "WWMI") and not _wwmi_dir_has_content(requested_wwmi)
    ):
        return old_wwmi, True
    return requested_wwmi, False


def _mark_previous_owner_migrated(old_base_root: Path, new_base_root: Path) -> str | None:
    """Update previous owner's ledger with informational migrated_to while preserving all files in A."""
    try:
        old_ledger = load_install_ledger(old_base_root)
        if not old_ledger:
            return None
        old_ledger.pop("pending_id", None)
        old_ledger.update(
            state="uninstalled",
            migrated_to=str(new_base_root),
            migrated_at=_dt.datetime.now(_dt.timezone.utc).isoformat(),
        )
        _write_json(_ledger_path(old_base_root), old_ledger)
        return None
    except (OSError, RuntimeError) as exc:
        return tr(
            f"新版本已提交生效，但无法更新旧目录账本（{old_base_root}）：{exc}",
            f"New version committed, but could not update previous ledger ({old_base_root}): {exc}",
        )


def _restored_source_root(ledger: dict | None, base_root: Path) -> Path | None:
    """Return resolved restored_from root when ledger carries an active rollback-source binding."""
    if not ledger:
        return None
    rf = ledger.get("restored_from")
    rpid = ledger.get("restored_pending_id")
    if not isinstance(rf, str) or not rf.strip() or not isinstance(rpid, str) or not rpid.strip():
        return None
    try:
        src = Path(rf).resolve()
        return src if src != base_root.resolve() else None
    except OSError:
        return None


def _restored_source_lock_target(ledger: dict | None, base_root: Path) -> Path | None:
    """Return resolved restored_from root for locking ONLY if its runtime/ledger still exists (never mkdir deleted folders)."""
    src = _restored_source_root(ledger, base_root)
    if src is None:
        return None
    try:
        return src if ((src / "runtime").is_dir() and _ledger_path(src).is_file()) else None
    except OSError:
        return None


def _ensure_restore_source_finalized(base_root: Path, ledger: dict | None) -> None:
    """Refuse modifying a rolled-back target A if source B's rolling_back ledger still holds the matching pending_id."""
    src_root = _restored_source_root(ledger, base_root)
    if src_root is None or not ledger:
        return
    try:
        st = src_root.stat()
    except FileNotFoundError:
        return
    if not stat.S_ISDIR(st.st_mode):
        return
    rpid = ledger.get("restored_pending_id")
    src_led = load_install_ledger(src_root)
    if (
        src_led
        and src_led.get("state") == "rolling_back"
        and src_led.get("pending_id") == rpid
        and Path(src_led["rollback_target"]).resolve() == base_root.resolve()
    ):
        raise RuntimeError(tr(
            f"检测到来源目录 {src_root} 的回滚收尾尚未完成，请先在该目录重试回滚完成后再操作当前目录。",
            f"Rollback finalization in {src_root} is still pending; retry Rollback there first before modifying this folder.",
        ))


def install_mount(
    source_root: Path,
    win64_dir: Path,
    wwmi_dir: Path,
    base_root: Path,
    platform_kind: str | None = None,
    skin_mode: str = "base",
    proton_utf8_guard: bool = False,
    compatdata_dir: Path | None = None,
    dry_run: bool = False,
    staging_root: Path | None = None,
    check_processes: bool = True,
    init_missing_wwmi: bool = False,
    migrate_from: Path | None = None,
    is_flatpak_steam: bool = False,
    skin_mode_explicit: bool = False,
) -> dict:
    """Sync the payload and commit the bootstrap (supports fresh install, in-place refresh, and verified A <-> B upgrade/rollback)."""
    p_kind = platform_kind or ("windows" if sys.platform == "win32" else "linux-proton")
    supplied_win64 = win64_dir.absolute()
    win64_dir = win64_dir.resolve()
    wwmi_dir = wwmi_dir.resolve()
    base_root = base_root.resolve()
    resolved_source = source_root.resolve()
    pkg_prov = read_package_provenance(resolved_source)
    if pkg_prov["version"] == "dev" and not pkg_prov["repository_commit"] and resolved_source != base_root:
        base_prov = read_package_provenance(base_root)
        if base_prov["version"] != "dev" or base_prov["repository_commit"]:
            pkg_prov = base_prov
    pkg_version = pkg_prov["version"] or "dev"
    pkg_commit = pkg_prov["repository_commit"]

    def preflight() -> tuple[dict | None, str, dict[str, str], VerifiedWin64Owner | None]:
        ledger = load_install_ledger(base_root)
        state, msg, owned, owner = _classify_mount(
            win64_dir, ledger,
            current_base_root=base_root,
            platform_kind=p_kind,
            migrate_from=migrate_from,
            package_version=pkg_version,
        )
        if state in ("active_elsewhere", "rolling_back"):
            raise RuntimeError(msg)
        return ledger, state, owned, owner

    # Basic target validation before reading/touching anything.
    _check_install_target(win64_dir, base_root, wwmi_dir, check_processes)
    if supplied_win64 != win64_dir:
        _check_install_target(supplied_win64, base_root, wwmi_dir, False)

    pre_ledger, pre_state, _, pre_owner = preflight()
    effective_wwmi, _ = _select_retained_wwmi(wwmi_dir, base_root, pre_owner, p_kind)
    if effective_wwmi != wwmi_dir:
        _check_install_target(win64_dir, base_root, effective_wwmi, False)
        if supplied_win64 != win64_dir:
            _check_install_target(supplied_win64, base_root, effective_wwmi, False)
    if not (effective_wwmi / "d3dx.ini").is_file() and not init_missing_wwmi:
        raise ValueError(f"无效的 WWMI 目录（缺少 d3dx.ini）：{effective_wwmi}")

    payload_map = resolve_payload_sources(resolved_source)
    engine_base = format_engine_path(base_root, p_kind)
    engine_wwmi = format_engine_path(effective_wwmi, p_kind)
    engine_runtime = format_engine_path(base_root / "runtime", p_kind)
    if "\n" in engine_wwmi or "\r" in engine_wwmi:
        raise ValueError("WWMI 路径不得含换行符")
    stub_data = build_win64_stub_reshade_ini(engine_base).encode("utf-8")

    def resolve_install_mode(old_led: dict | None, mount_st: str, owner: VerifiedWin64Owner | None) -> str:
        if owner is not None:
            return _classify_takeover_kind(owner, base_root, old_led, pkg_version)
        if mount_st == "installed":
            return "refresh"
        if mount_st in ("drifted", "installing"):
            return "repair"
        return "fresh"

    if dry_run:
        options, warning = proton_launch_options(base_root, proton_utf8_guard) if p_kind == "linux-proton" else (None, None)
        mode_name = resolve_install_mode(pre_ledger, pre_state, pre_owner)
        return {
            "mode": "dry-run", "install_mode": mode_name, "platform_kind": p_kind,
            "base_root": str(base_root), "win64_dir": str(win64_dir), "wwmi_dir": str(effective_wwmi),
            "engine_base_path": engine_base, "engine_wwmi_dir": engine_wwmi,
            "win64_bootstrap_files": [str(win64_dir / name) for name in BOOTSTRAP_FILES],
            "external_payload_count": len(payload_map),
            "installed_version": pkg_version,
            "upgraded_from": str(pre_owner.base_root) if pre_owner else None,
            "proton_launch_options": options, "proton_preload_warning": warning,
        }

    stage_root = staging_root or default_staging_root(base_root)
    pre_restore_src = _restored_source_lock_target(pre_ledger, base_root)
    with _win64_lock(win64_dir), _base_root_locks(base_root, pre_restore_src):
        _check_install_target(win64_dir, base_root, effective_wwmi, check_processes)
        old_ledger, mount_state, owned, verified_owner = preflight()
        cur_restore_src = _restored_source_lock_target(old_ledger, base_root)
        owner_restore_src = _restored_source_lock_target(verified_owner.ledger, verified_owner.base_root) if verified_owner else None
        extra_locks = [
            p for p in (
                verified_owner.base_root if verified_owner is not None else None,
                cur_restore_src if cur_restore_src != pre_restore_src else None,
                owner_restore_src,
            )
            if p is not None and p != base_root and p != pre_restore_src
        ]
        with _base_root_locks(*extra_locks):
            _ensure_restore_source_finalized(base_root, old_ledger)
            if verified_owner is not None:
                _ensure_restore_source_finalized(verified_owner.base_root, verified_owner.ledger)
            effective_wwmi, retained_wwmi = _select_retained_wwmi(wwmi_dir, base_root, verified_owner, p_kind)
            if effective_wwmi != wwmi_dir:
                _check_install_target(win64_dir, base_root, effective_wwmi, False)
            if not (effective_wwmi / "d3dx.ini").is_file() and not init_missing_wwmi:
                raise ValueError(f"无效的 WWMI 目录（缺少 d3dx.ini）：{effective_wwmi}")

            engine_wwmi = format_engine_path(effective_wwmi, p_kind)
            if "\n" in engine_wwmi or "\r" in engine_wwmi:
                raise ValueError("WWMI 路径不得含换行符")

            abandoned = _pending_files(win64_dir, old_ledger)
            if abandoned:
                stage_files_for_removal(abandoned, stage_root, "wwmi-dx12-interrupted-install")

            install_mode = resolve_install_mode(old_ledger, mount_state, verified_owner)
            previous_owner_root: Path | None = verified_owner.base_root if verified_owner is not None else None
            if previous_owner_root is None and old_ledger and old_ledger.get("state") == "installing":
                prev_raw = old_ledger.get("previous_owner")
                if isinstance(prev_raw, str) and prev_raw.strip():
                    previous_owner_root = Path(prev_raw).resolve()
            preserved_upgraded_from: str | None = str(previous_owner_root) if previous_owner_root is not None else None
            if preserved_upgraded_from is None and old_ledger:
                uf_raw = old_ledger.get("upgraded_from")
                if isinstance(uf_raw, str) and uf_raw.strip():
                    preserved_upgraded_from = uf_raw.strip()

            # ---- Step 1: Prepare B completely BEFORE mutating Win64 ----
            migration_warnings: list[str] = []
            migrated_cache: list[str] = []
            migrated_addons: list[str] = []

            if init_missing_wwmi and not (effective_wwmi / "d3dx.ini").is_file():
                init_wwmi_skeleton(effective_wwmi)

            dxgi_data = payload_map["host/ReShade64-game-forward.dll"].read_bytes()
            for rel, src in payload_map.items():
                dst = base_root / rel
                dst.parent.mkdir(parents=True, exist_ok=True)
                if dst != src:
                    _atomic_write(dst, dxgi_data if rel == "host/ReShade64-game-forward.dll" else src.read_bytes())

            src_prov = resolved_source / "BUILD-PROVENANCE.json"
            dst_prov = base_root / "BUILD-PROVENANCE.json"
            if resolved_source != base_root and _regular_file(src_prov):
                _atomic_write(dst_prov, src_prov.read_bytes())

            for sub in ("runtime/logs", "runtime/cache/shaders", "runtime/seed", "runtime/recovery", "seed"):
                (base_root / sub).mkdir(parents=True, exist_ok=True)

            # Merge external ReShade.ini (prefer existing B/ReShade.ini; otherwise migrate from A/ReShade.ini)
            ext_reshade_path = base_root / "ReShade.ini"
            reshade_source_text: str | None = None
            if _regular_file(ext_reshade_path):
                reshade_source_text = ext_reshade_path.read_text(encoding="utf-8", errors="ignore")
            elif verified_owner is not None and _regular_file(verified_owner.base_root / "ReShade.ini"):
                reshade_source_text = (verified_owner.base_root / "ReShade.ini").read_text(encoding="utf-8", errors="ignore")
            merged_reshade = merge_external_reshade_ini(reshade_source_text)
            _atomic_write(ext_reshade_path, merged_reshade.encode("utf-8"))
            if verified_owner is not None:
                migration_warnings.extend(
                    scan_reshade_ini_migration_warnings(merged_reshade, verified_owner.base_root, p_kind)
                )

            # Merge wwmi_dx12.ini preserving UTF-16LE encoding and user keys
            ext_wwmi_ini = base_root / "wwmi_dx12.ini"
            existing_wwmi_bytes: bytes | None = None
            old_engine_base: str | None = None
            if _regular_file(ext_wwmi_ini):
                existing_wwmi_bytes = ext_wwmi_ini.read_bytes()
                old_engine_base = engine_base
            elif verified_owner is not None and _regular_file(verified_owner.base_root / "wwmi_dx12.ini"):
                existing_wwmi_bytes = (verified_owner.base_root / "wwmi_dx12.ini").read_bytes()
                old_engine_base = format_engine_path(verified_owner.base_root, p_kind)
            merged_wwmi_bytes = merge_wwmi_dx12_ini(
                existing_wwmi_bytes,
                engine_wwmi,
                engine_runtime,
                skin_mode=skin_mode,
                old_engine_base=old_engine_base,
                force_skin_mode=skin_mode_explicit,
            )
            _atomic_write(ext_wwmi_ini, merged_wwmi_bytes)

            # Migrate compatible caches & third-party *.addon64 from A (excluding WWMI-owned payload)
            if verified_owner is not None:
                old_rt = resolve_old_runtime_dir(verified_owner.base_root, p_kind)
                migrated_cache = migrate_cache_files(old_rt, base_root / "runtime")
                migrated_addons, addon_warnings = migrate_third_party_addons(verified_owner.base_root, base_root)
                migration_warnings.extend(addon_warnings)

            if p_kind == "linux-proton":
                migration_warnings.extend(
                    _check_flatpak_visibility_warnings(base_root, effective_wwmi, compatdata_dir, is_flatpak_steam)
                )
                if verified_owner is not None:
                    migration_warnings.append(tr(
                        f"外部挂载根目录已从 {verified_owner.base_root} 变更至 {base_root}；"
                        "若 Steam 启动选项使用了 LD_PRELOAD（wine-filename-utf8.so），请按需同步更新路径（本工具不会自动修改 Steam 配置或 XXMI）。",
                        f"External base root changed from {verified_owner.base_root} to {base_root}; "
                        "if your Steam Launch Options use LD_PRELOAD (wine-filename-utf8.so), update the path accordingly (Steam/XXMI configs are never modified automatically).",
                    ))

            # ---- Step 2: Transactional 2-file Win64 swap with recovery manifest & auto-rollback ----
            if check_processes and (running := running_watched_processes()):
                raise RuntimeError(f"检测到游戏正在运行（{', '.join(running)}），请退出后重试安装")

            # Re-verify Win64 ownership right before creating recovery backup & mutating Win64
            if verified_owner is not None:
                re_owner, reject_reason = detect_verified_win64_owner(win64_dir, base_root, p_kind, migrate_from)
                if re_owner is None or re_owner.base_root != verified_owner.base_root:
                    raise RuntimeError(reject_reason or tr(
                        "升级期间 Win64 文件状态发生变化，已中止写入以防损坏。",
                        "Win64 bootstrap files changed before swap; aborted to avoid corruption.",
                    ))
                owned = re_owner.owned_hashes
            else:
                owned = _owned_bootstrap(
                    win64_dir,
                    old_ledger if old_ledger and old_ledger["state"] in ACTIVE_STATES else None,
                    base_root=base_root,
                )

            bootstrap = dict(zip(BOOTSTRAP_FILES, (dxgi_data, stub_data)))
            expected_hashes = {name: hashlib.sha256(data).hexdigest() for name, data in bootstrap.items()}
            pending_id = uuid.uuid4().hex
            recovery_dir: Path | None = None
            recovery_backups: dict[str, Path] = {}
            preserved_prev_hashes: dict[str, str] = {}

            # When resuming an interrupted `installing` transaction, MUST reuse the original
            # verified recovery backups and original previous_sha256 instead of resampling the
            # mixed Win64 state into a poisoned backup.
            resuming_installing = (
                verified_owner is None
                and old_ledger is not None
                and old_ledger.get("state") == "installing"
            )
            if resuming_installing:
                old_recs = {item["relative"]: item for item in old_ledger["win64_files"]}
                if isinstance(old_ledger.get("recovery_dir"), str) and old_ledger["recovery_dir"].strip():
                    cand_rec_dir = Path(old_ledger["recovery_dir"]).resolve()
                    if cand_rec_dir.is_relative_to((base_root / "runtime/recovery").resolve()) and cand_rec_dir.is_dir():
                        recovery_dir = cand_rec_dir
                for n in BOOTSTRAP_FILES:
                    old_rec = old_recs.get(n, {})
                    bkp = _verified_recovery_backup_path(old_rec, base_root)
                    if bkp is not None:
                        recovery_backups[n] = bkp
                        preserved_prev_hashes[n] = old_rec["previous_sha256"]
                        if recovery_dir is None:
                            recovery_dir = bkp.parent
                    elif n in owned and "previous_sha256" in old_rec and not old_ledger.get("previous_owner"):
                        preserved_prev_hashes[n] = old_rec["previous_sha256"]
                # If retrying with a DIFFERENT payload (B2) while Win64 still holds B1's partial file,
                # reset that B1 file back to the verified pre-transaction state BEFORE rewriting the ledger
                # so an interruption before B2's first swap can never orphan B1 on disk.
                for n in BOOTSTRAP_FILES:
                    if n in owned and owned[n] != expected_hashes[n] and owned[n] != preserved_prev_hashes.get(n):
                        if n in recovery_backups:
                            rb_tmp = win64_dir / f".wwmi-reset-{uuid.uuid4().hex[:12]}-{n}.tmp"
                            _write_recovery_file(rb_tmp, recovery_backups[n].read_bytes())
                            os.replace(rb_tmp, win64_dir / n)
                            owned[n] = preserved_prev_hashes[n]
                        else:
                            stage_files_for_removal([win64_dir / n], stage_root, "wwmi-dx12-reset-interrupted-payload")
                            del owned[n]
            elif owned:
                recovery_dir, recovery_backups = _create_recovery_backup(
                    win64_dir, [n for n in BOOTSTRAP_FILES if n in owned], base_root, pending_id,
                )
                preserved_prev_hashes = dict(owned)

            ledger: dict = {
                "schema_version": 1, "state": "installing", "pending_id": pending_id,
                "platform_kind": p_kind, "base_root": str(base_root),
                "win64_dir": str(win64_dir), "wwmi_dir": str(effective_wwmi),
                "installed_version": pkg_version,
                "installed_commit": pkg_commit,
                "win64_files": [
                    {
                        "relative": name,
                        "path": str(win64_dir / name),
                        "sha256": expected_hashes[name],
                        "size": len(data),
                        **({"previous_sha256": preserved_prev_hashes[name]} if name in preserved_prev_hashes else {}),
                        **({"recovery_backup": str(recovery_backups[name])} if name in recovery_backups else {}),
                    }
                    for name, data in bootstrap.items()
                ],
                "extra_managed_files": old_ledger.get("extra_managed_files", []) if old_ledger else [],
            }
            if previous_owner_root is not None:
                ledger["previous_owner"] = str(previous_owner_root)
            if preserved_upgraded_from is not None:
                ledger["upgraded_from"] = preserved_upgraded_from
            if recovery_dir is not None:
                ledger["recovery_dir"] = str(recovery_dir)
            ledger_path = _ledger_path(base_root)
            _write_json(ledger_path, ledger)

            swapped: list[str] = []
            try:
                for name, data in bootstrap.items():
                    dst = win64_dir / name
                    temp = win64_dir / f".wwmi-{pending_id}-{name}.tmp"
                    _write_new_file(temp, data)
                    if name not in owned:
                        if os.path.lexists(dst):
                            raise RuntimeError(f"安装期间已有其他程序创建 {dst}，拒绝覆盖；可在同一目录重试")
                        if sys.platform == "win32":
                            temp.rename(dst)
                        else:
                            os.replace(temp, dst)
                    else:
                        os.replace(temp, dst)
                    swapped.append(name)
            except BaseException:
                # Automatically roll back ALL files covered by original recovery backups (including
                # any file already overwritten before an earlier crash when resuming an upgrade).
                if recovery_backups and (swapped or resuming_installing):
                    restored_ok = True
                    for name in reversed(BOOTSTRAP_FILES):
                        bkp = recovery_backups.get(name)
                        if not bkp or not bkp.is_file():
                            continue
                        try:
                            rb_tmp = win64_dir / f".wwmi-rollback-{uuid.uuid4().hex[:12]}-{name}.tmp"
                            _write_recovery_file(rb_tmp, bkp.read_bytes())
                            os.replace(rb_tmp, win64_dir / name)
                        except OSError:
                            restored_ok = False
                    # Move any leftover pending .tmp into recovery_dir/aborted-tmp (never unlink directly)
                    if recovery_dir is not None:
                        aborted_dir = recovery_dir / "aborted-tmp"
                        for name in BOOTSTRAP_FILES:
                            leftover = win64_dir / f".wwmi-{pending_id}-{name}.tmp"
                            if os.path.lexists(leftover):
                                try:
                                    aborted_dir.mkdir(parents=True, exist_ok=True)
                                    shutil.move(str(leftover), str(aborted_dir / leftover.name))
                                except OSError:
                                    restored_ok = False
                    if restored_ok and preserved_prev_hashes:
                        try:
                            all_match = all(
                                _regular_file(win64_dir / n) and sha256_file(win64_dir / n) == h
                                for n, h in preserved_prev_hashes.items()
                            )
                        except (OSError, RuntimeError):
                            all_match = False
                        if all_match:
                            if previous_owner_root is not None:
                                _write_json(ledger_path, {
                                    "schema_version": 1,
                                    "state": "uninstalled",
                                    "win64_dir": str(win64_dir),
                                    "recovered_to": str(previous_owner_root),
                                    "recovered_at": _dt.datetime.now(_dt.timezone.utc).isoformat(),
                                })
                            elif old_ledger and old_ledger.get("state") == "installed":
                                _write_json(ledger_path, old_ledger)
                raise

            # ---- Step 3: Commit B ownership and mark A migrated_to (never deleting A) ----
            ledger.pop("pending_id", None)
            ledger.pop("previous_owner", None)
            ledger.pop("recovery_dir", None)
            for rec in ledger["win64_files"]:
                rec.pop("previous_sha256", None)
                rec.pop("recovery_backup", None)
            ledger.update(
                state="installed",
                installed_at=_dt.datetime.now(_dt.timezone.utc).isoformat(),
                install_mode=install_mode,
            )
            if preserved_upgraded_from is not None:
                ledger["upgraded_from"] = preserved_upgraded_from
            _write_json(ledger_path, ledger)

            if previous_owner_root is not None:
                warn = _mark_previous_owner_migrated(previous_owner_root, base_root)
                if warn:
                    migration_warnings.append(warn)

            result = dict(ledger)
            result["migration_warnings"] = migration_warnings
            result["migrated_cache"] = migrated_cache
            result["migrated_addons"] = migrated_addons
            result["retained_wwmi_dir"] = retained_wwmi
            if p_kind == "linux-proton":
                result["proton_launch_options"], result["proton_preload_warning"] = proton_launch_options(
                    base_root, proton_utf8_guard,
                )
            return result


def rollback_mount(
    base_root: Path,
    target_root: Path | None = None,
    dry_run: bool = False,
    staging_root: Path | None = None,
    check_processes: bool = True,
) -> dict:
    """Roll back an installed mount from NEW folder B back to previous folder A (crash-resumable, works for v1.0.2/v1.0.3 A)."""
    base_root = base_root.resolve()
    ledger_b = load_install_ledger(base_root)
    if not ledger_b or ledger_b.get("state") not in ("installed", "rolling_back"):
        raise RuntimeError(tr(
            f"当前目录 {base_root} 未处于已安装或待恢复回滚状态，无法执行回滚。",
            f"Current folder {base_root} is not in an active installed or rolling_back state; cannot roll back.",
        ))
    if ledger_b["state"] == "rolling_back":
        recorded_target = Path(ledger_b["rollback_target"]).resolve()
        if target_root is not None and target_root.resolve() != recorded_target:
            raise RuntimeError(tr(
                f"当前存在指向 {recorded_target} 的未完成回滚，请先重试完成该回滚。",
                f"An interrupted rollback to {recorded_target} is pending; retry rollback to that target first.",
            ))
        target_a = recorded_target
    else:
        prev_raw = str(target_root.resolve()) if target_root is not None else ledger_b.get("upgraded_from")
        if not isinstance(prev_raw, str) or not prev_raw.strip():
            raise RuntimeError(tr(
                "当前账本未记录升级来源目录（upgraded_from）；若需回滚请通过 --to <旧目录> 指定。",
                "No previous folder (upgraded_from) recorded in ledger; specify --to <old_dir> to roll back.",
            ))
        target_a = Path(prev_raw).resolve()
    if target_a == base_root:
        raise RuntimeError(tr("回滚目标目录不能与当前目录相同。", "Rollback target cannot be the same as the current folder."))
    if not target_a.is_dir():
        raise RuntimeError(tr(f"旧版目录不存在，无法回滚：{target_a}", f"Previous folder does not exist: {target_a}"))
    host_dll_a = target_a / "host/ReShade64-game-forward.dll"
    if not _regular_file(host_dll_a):
        raise RuntimeError(tr(
            f"旧版目录缺少 host/ReShade64-game-forward.dll，无法回滚：{target_a}",
            f"Previous folder is missing host/ReShade64-game-forward.dll; cannot roll back: {target_a}",
        ))
    p_kind = ledger_b.get("platform_kind") or ("windows" if sys.platform == "win32" else "linux-proton")
    win64_dir = Path(ledger_b["win64_dir"]).resolve()
    if Path(ledger_b["win64_dir"]) != win64_dir:
        raise RuntimeError(f"原 Win64 目录现在指向其他位置，拒绝回滚：{ledger_b['win64_dir']}")
    if check_processes and (running := running_watched_processes()):
        raise RuntimeError(f"检测到游戏正在运行（{', '.join(running)}），请退出后重试回滚")

    def _validate_target_a(led_a: dict | None, resuming: bool, led_b_cur: dict) -> bool:
        """Validate target_a ledger; returns True if A was already committed by an interrupted rollback."""
        if led_a and led_a.get("state") == "installed":
            if (
                resuming
                and Path(led_a["win64_dir"]).resolve() == win64_dir
                and led_a.get("restored_from") == str(base_root)
                and led_a.get("restored_pending_id") == led_b_cur.get("pending_id")
            ):
                recs_a = {item["relative"]: item for item in led_a["win64_files"]}
                if all(
                    _regular_file(win64_dir / n) and sha256_file(win64_dir / n) == recs_a[n]["sha256"]
                    for n in BOOTSTRAP_FILES
                ):
                    return True
            # Allow overwriting stale A ledger left in `installed` if marking A migrated failed during upgrade,
            # ONLY when A is bound to THIS same Win64, B explicitly records upgraded_from=A, and B owns both Win64 files.
            uf_raw = led_b_cur.get("upgraded_from")
            if (
                not resuming
                and Path(led_a["win64_dir"]).resolve() == win64_dir
                and isinstance(uf_raw, str)
                and uf_raw.strip()
                and Path(uf_raw).resolve() == target_a
            ):
                try:
                    if len(_owned_bootstrap(win64_dir, led_b_cur, base_root=base_root)) == len(BOOTSTRAP_FILES):
                        return False
                except RuntimeError:
                    pass
            raise RuntimeError(tr(
                f"旧目录 {target_a} 当前已处于活动安装状态（绑定 {led_a.get('win64_dir')}），拒绝覆盖其绑定。",
                f"Previous folder {target_a} is actively installed (bound to {led_a.get('win64_dir')}); refusing rollback.",
            ))
        if led_a and led_a.get("state") in PENDING_STATES:
            raise RuntimeError(tr(
                f"旧目录 {target_a} 的账本处于未完成状态（{led_a['state']}），拒绝回滚。",
                f"Previous folder {target_a} has an interrupted ledger (state={led_a['state']}); refusing rollback.",
            ))
        if not led_a and not _regular_file(target_a / "wwmi_dx12.ini"):
            raise RuntimeError(tr(
                f"旧目录 {target_a} 缺少安装账本和 wwmi_dx12.ini，无法确认为可回滚的 WWMI-DX12-RT 目录。",
                f"Previous folder {target_a} lacks both install ledger and wwmi_dx12.ini; refusing rollback.",
            ))
        if led_a and led_a.get("win64_dir") and Path(led_a["win64_dir"]).resolve() != win64_dir:
            raise RuntimeError(tr(
                f"旧目录 {target_a} 的账本绑定在 {led_a['win64_dir']}，与当前 Win64 不一致，拒绝回滚。",
                f"Previous folder {target_a} ledger is bound to {led_a['win64_dir']}, which does not match Win64.",
            ))
        return False

    pre_ledger_a = load_install_ledger(target_a)
    _validate_target_a(pre_ledger_a, ledger_b["state"] == "rolling_back", ledger_b)

    if dry_run:
        _owned_bootstrap(win64_dir, ledger_b, base_root=base_root)
        return {
            "mode": "dry-run",
            "action": "rollback",
            "from_root": str(base_root),
            "to_root": str(target_a),
            "win64_dir": str(win64_dir),
            "target_version": format_installed_version(pre_ledger_a) if pre_ledger_a else tr("旧版（版本未知）", "Legacy (version unknown)"),
        }

    stage_root = staging_root or default_staging_root(base_root)
    pre_restore_src = _restored_source_lock_target(ledger_b, base_root)
    with _win64_lock(win64_dir), _base_root_locks(base_root, target_a, pre_restore_src):
        ledger_b = load_install_ledger(base_root)
        if not ledger_b or ledger_b.get("state") not in ("installed", "rolling_back"):
            raise RuntimeError(tr("加锁后账本状态已变化，请重试回滚。", "Ledger state changed while acquiring lock; retry rollback."))
        if Path(ledger_b["win64_dir"]).resolve() != win64_dir:
            raise RuntimeError(tr("Win64 目录在加锁期间发生变化，拒绝回滚。", "Win64 target changed while acquiring lock."))
        resuming_rollback = ledger_b["state"] == "rolling_back"
        if resuming_rollback and Path(ledger_b["rollback_target"]).resolve() != target_a:
            raise RuntimeError(tr("加锁后回滚目标发生变化，请重试回滚。", "Rollback target changed while acquiring lock."))
        if check_processes and (running := running_watched_processes()):
            raise RuntimeError(f"检测到游戏正在运行（{', '.join(running)}），请退出后重试回滚")
        if not _regular_file(host_dll_a):
            raise RuntimeError(tr(
                f"旧版目录缺少 host/ReShade64-game-forward.dll，无法回滚：{target_a}",
                f"Previous folder is missing host/ReShade64-game-forward.dll; cannot roll back: {target_a}",
            ))

        cur_restore_src = _restored_source_lock_target(ledger_b, base_root)
        extra_rb = [
            p for p in (cur_restore_src,)
            if p is not None and p not in (base_root, target_a, pre_restore_src)
        ]
        with _base_root_locks(*extra_rb):
            _ensure_restore_source_finalized(base_root, ledger_b)

            ledger_a = load_install_ledger(target_a)
            already_committed_a = _validate_target_a(ledger_a, resuming_rollback, ledger_b)

            abandoned = _pending_files(win64_dir, ledger_b)
            if abandoned:
                stage_files_for_removal(abandoned, stage_root, "wwmi-dx12-interrupted-rollback")

            if already_committed_a and ledger_a is not None:
                ledger_b.pop("pending_id", None)
                ledger_b.pop("rollback_target", None)
                ledger_b.pop("recovery_dir", None)
                for rec in ledger_b.get("win64_files", []):
                    rec.pop("previous_sha256", None)
                    rec.pop("recovery_backup", None)
                ledger_b.update(
                    state="uninstalled",
                    rolled_back_to=str(target_a),
                    uninstalled_at=_dt.datetime.now(_dt.timezone.utc).isoformat(),
                )
                _write_json(_ledger_path(base_root), ledger_b)
                res_fast: dict = {
                    "state": "rolled_back",
                    "from_root": str(base_root),
                    "to_root": str(target_a),
                    "win64_dir": str(win64_dir),
                    "target_version": format_installed_version(ledger_a),
                }
                if p_kind == "linux-proton":
                    res_fast["proton_launch_options"], res_fast["proton_preload_warning"] = proton_launch_options(target_a, False)
                return res_fast

            engine_base_a = format_engine_path(target_a, p_kind)
            dxgi_a = host_dll_a.read_bytes()
            stub_a = build_win64_stub_reshade_ini(engine_base_a).encode("utf-8")
            bootstrap_a = dict(zip(BOOTSTRAP_FILES, (dxgi_a, stub_a)))
            expected_a = {name: hashlib.sha256(data).hexdigest() for name, data in bootstrap_a.items()}

            owned_b = _owned_bootstrap(win64_dir, ledger_b, base_root=base_root)
            if len(owned_b) != len(BOOTSTRAP_FILES):
                raise RuntimeError(tr(
                    "当前 Win64 引导文件不完整，请先修复当前安装或卸载后再处理。",
                    "Current Win64 bootstrap files are incomplete; repair or uninstall first.",
                ))

            # If target_a still carries a stale `installed` ledger (from an earlier read-only upgrade warning),
            # mark it inactive BEFORE writing B's `rolling_back` ledger or touching Win64 so any mid-swap crash
            # remains cleanly resumable (and if A is still unwritable, abort cleanly before mutating Win64).
            if not resuming_rollback and ledger_a and ledger_a.get("state") == "installed":
                stale_a = dict(ledger_a)
                stale_a.pop("pending_id", None)
                stale_a.update(
                    state="uninstalled",
                    migrated_to=str(base_root),
                    migrated_at=_dt.datetime.now(_dt.timezone.utc).isoformat(),
                )
                _write_json(_ledger_path(target_a), stale_a)
                ledger_a = stale_a

            pending_id = uuid.uuid4().hex
            recovery_dir: Path | None = None
            recovery_backups: dict[str, Path] = {}
            preserved_prev_hashes: dict[str, str] = {}
            if resuming_rollback:
                old_recs = {item["relative"]: item for item in ledger_b["win64_files"]}
                if isinstance(ledger_b.get("recovery_dir"), str) and ledger_b["recovery_dir"].strip():
                    cand_rec = Path(ledger_b["recovery_dir"]).resolve()
                    if cand_rec.is_relative_to((base_root / "runtime/recovery").resolve()) and cand_rec.is_dir():
                        recovery_dir = cand_rec
                for n in BOOTSTRAP_FILES:
                    bkp = _verified_recovery_backup_path(old_recs[n], base_root)
                    if bkp is not None:
                        recovery_backups[n] = bkp
                        preserved_prev_hashes[n] = old_recs[n]["previous_sha256"]
                        if recovery_dir is None:
                            recovery_dir = bkp.parent
                if len(recovery_backups) != len(BOOTSTRAP_FILES) and any(owned_b[n] != expected_a[n] for n in BOOTSTRAP_FILES):
                    raise RuntimeError(tr(
                        "未完成回滚的原始恢复备份缺失或损坏，拒绝覆盖 Win64。",
                        "Original recovery backups for interrupted rollback are missing or corrupted; refusing to modify Win64.",
                    ))
                for n in BOOTSTRAP_FILES:
                    if owned_b[n] != expected_a[n] and owned_b[n] != preserved_prev_hashes.get(n):
                        rb_tmp = win64_dir / f".wwmi-reset-{uuid.uuid4().hex[:12]}-{n}.tmp"
                        _write_recovery_file(rb_tmp, recovery_backups[n].read_bytes())
                        os.replace(rb_tmp, win64_dir / n)
                        owned_b[n] = preserved_prev_hashes[n]
            else:
                recovery_dir, recovery_backups = _create_recovery_backup(win64_dir, BOOTSTRAP_FILES, base_root, pending_id)
                preserved_prev_hashes = dict(owned_b)

            restore_b_installed = dict(ledger_b)
            restore_b_installed.pop("pending_id", None)
            restore_b_installed.pop("rollback_target", None)
            restore_b_installed.pop("recovery_dir", None)
            restore_b_installed["state"] = "installed"
            if preserved_prev_hashes and len(preserved_prev_hashes) == len(BOOTSTRAP_FILES):
                restore_b_installed["win64_files"] = [
                    {
                        "relative": name,
                        "path": str(win64_dir / name),
                        "sha256": preserved_prev_hashes[name],
                        "size": recovery_backups[name].stat().st_size if name in recovery_backups else 0,
                    }
                    for name in BOOTSTRAP_FILES
                ]

            # Persist rolling_back transaction in B BEFORE mutating Win64 so any crash is resumable.
            all_already_a = resuming_rollback and all(owned_b.get(n) == expected_a[n] for n in BOOTSTRAP_FILES)
            commit_pending_id = ledger_b["pending_id"] if all_already_a else pending_id
            if not all_already_a:
                rb_ledger_b = dict(ledger_b)
                rb_ledger_b.update(
                    schema_version=1,
                    state="rolling_back",
                    pending_id=pending_id,
                    rollback_target=str(target_a),
                    win64_files=[
                        {
                            "relative": name,
                            "path": str(win64_dir / name),
                            "sha256": expected_a[name],
                            "size": len(data),
                            "previous_sha256": preserved_prev_hashes[name],
                            "recovery_backup": str(recovery_backups[name]),
                        }
                        for name, data in bootstrap_a.items()
                    ],
                )
                if recovery_dir is not None:
                    rb_ledger_b["recovery_dir"] = str(recovery_dir)
                _write_json(_ledger_path(base_root), rb_ledger_b)

                try:
                    for name, data in bootstrap_a.items():
                        if owned_b.get(name) == expected_a[name]:
                            continue
                        temp = win64_dir / f".wwmi-{pending_id}-{name}.tmp"
                        _write_new_file(temp, data)
                        os.replace(temp, win64_dir / name)
                except BaseException:
                    if recovery_backups:
                        for name in reversed(BOOTSTRAP_FILES):
                            bkp = recovery_backups.get(name)
                            target_p = win64_dir / name
                            prev_h = preserved_prev_hashes.get(name)
                            if not bkp or not bkp.is_file():
                                continue
                            try:
                                if prev_h and _regular_file(target_p) and sha256_file(target_p) == prev_h:
                                    continue
                                rb_tmp = win64_dir / f".wwmi-rollback-{uuid.uuid4().hex[:12]}-{name}.tmp"
                                _write_recovery_file(rb_tmp, bkp.read_bytes())
                                os.replace(rb_tmp, target_p)
                            except (OSError, RuntimeError):
                                pass
                        if recovery_dir is not None:
                            aborted_dir = recovery_dir / "aborted-tmp"
                            for name in BOOTSTRAP_FILES:
                                leftover = win64_dir / f".wwmi-{pending_id}-{name}.tmp"
                                if os.path.lexists(leftover):
                                    try:
                                        aborted_dir.mkdir(parents=True, exist_ok=True)
                                        shutil.move(str(leftover), str(aborted_dir / leftover.name))
                                    except OSError:
                                        pass
                        if len(preserved_prev_hashes) == len(BOOTSTRAP_FILES):
                            try:
                                all_match = all(
                                    _regular_file(win64_dir / n) and sha256_file(win64_dir / n) == h
                                    for n, h in preserved_prev_hashes.items()
                                )
                            except (OSError, RuntimeError):
                                all_match = False
                            if all_match:
                                _write_json(_ledger_path(base_root), restore_b_installed)
                    raise

            # Commit A ledger back to installed (preserving v1.0.2/v1.0.3 compatibility) and mark B uninstalled.
            restored_ledger_a = dict(ledger_a) if ledger_a else {"schema_version": 1, "platform_kind": p_kind, "extra_managed_files": []}
            restored_wwmi = resolve_active_wwmi_dir_from_base(target_a, ledger_a, p_kind)
            restored_ledger_a.pop("pending_id", None)
            restored_ledger_a.pop("migrated_to", None)
            restored_ledger_a.pop("migrated_at", None)
            restored_ledger_a.update(
                schema_version=1,
                state="installed",
                platform_kind=p_kind,
                base_root=str(target_a),
                win64_dir=str(win64_dir),
                wwmi_dir=str(restored_wwmi) if restored_wwmi else restored_ledger_a.get("wwmi_dir", ledger_b.get("wwmi_dir", "")),
                win64_files=[
                    {
                        "relative": name,
                        "path": str(win64_dir / name),
                        "sha256": expected_a[name],
                        "size": len(data),
                    }
                    for name, data in bootstrap_a.items()
                ],
                restored_from=str(base_root),
                restored_pending_id=commit_pending_id,
                restored_at=_dt.datetime.now(_dt.timezone.utc).isoformat(),
            )
            _write_json(_ledger_path(target_a), restored_ledger_a)

            final_b = dict(restore_b_installed)
            final_b.update(
                state="uninstalled",
                rolled_back_to=str(target_a),
                uninstalled_at=_dt.datetime.now(_dt.timezone.utc).isoformat(),
            )
            _write_json(_ledger_path(base_root), final_b)

            res: dict = {
                "state": "rolled_back",
                "from_root": str(base_root),
                "to_root": str(target_a),
                "win64_dir": str(win64_dir),
                "target_version": format_installed_version(restored_ledger_a),
            }
            if p_kind == "linux-proton":
                res["proton_launch_options"], res["proton_preload_warning"] = proton_launch_options(target_a, False)
            return res


def uninstall_mount(
    base_root: Path,
    dry_run: bool = False,
    staging_root: Path | None = None,
    check_processes: bool = True,
) -> dict:
    """Stage only bootstrap files with a verified name, location and digest (or restore previous owner if aborting interrupted upgrade)."""
    base_root = base_root.resolve()
    ledger_path = _ledger_path(base_root)
    noop = {
        "state": "noop",
        "message": tr(
            f"未发现活动中的安装账本（{ledger_path}）",
            f"No active installation ledger found ({ledger_path})",
        ),
    }

    def preview(ledger: dict) -> list[Path]:
        win64 = Path(ledger["win64_dir"])
        if win64 != win64.resolve():
            raise RuntimeError(f"原 Win64 目录现在指向其他位置，拒绝卸载：{win64}")
        if check_processes and (running := running_watched_processes()):
            raise RuntimeError(f"检测到游戏正在运行（{', '.join(running)}），请先正常退出游戏再执行卸载")
        return [win64 / name for name in _owned_bootstrap(win64, ledger, base_root=base_root)] + _pending_files(win64, ledger)

    ledger = load_install_ledger(base_root)
    if not ledger or ledger["state"] not in ACTIVE_STATES:
        return noop
    if ledger["state"] == "rolling_back":
        raise RuntimeError(tr(
            "检测到未完成的回滚事务，请先重试回滚完成后再按需卸载。",
            "An interrupted rollback transaction is pending; retry Rollback first before uninstalling.",
        ))
    if dry_run:
        return {"state": "dry-run", "will_stage": [str(p) for p in preview(ledger)]}

    lock_win64 = Path(ledger["win64_dir"]).resolve()
    restore_src = _restored_source_lock_target(ledger, base_root)
    with _win64_lock(lock_win64), _base_root_locks(base_root, restore_src):
        ledger = load_install_ledger(base_root)
        if not ledger or ledger["state"] not in ACTIVE_STATES:
            return noop
        if ledger["state"] == "rolling_back":
            raise RuntimeError(tr(
                "检测到未完成的回滚事务，请先重试回滚完成后再按需卸载。",
                "An interrupted rollback transaction is pending; retry Rollback first before uninstalling.",
            ))
        cur_restore_src = _restored_source_lock_target(ledger, base_root)
        extra_rb_lock = [cur_restore_src] if cur_restore_src is not None and cur_restore_src != restore_src else []
        with _base_root_locks(*extra_rb_lock):
            _ensure_restore_source_finalized(base_root, ledger)
            if Path(ledger["win64_dir"]).resolve() != lock_win64:
                raise RuntimeError(tr(
                    "安装账本绑定的 Win64 目录在加锁期间发生变化，请重试卸载。",
                    "Ledger Win64 target changed while acquiring lock; retry Uninstall.",
                ))
            to_remove = preview(ledger)
            win64 = Path(ledger["win64_dir"])
            stage_root = staging_root or default_staging_root(base_root)

            # If aborting an interrupted side-by-side upgrade with verified recovery backups, restore previous owner cleanly.
            prev_owner = ledger.get("previous_owner")
            if ledger["state"] == "installing" and prev_owner:
                records = {item["relative"]: item for item in ledger["win64_files"]}
                verified_bkps = {n: _verified_recovery_backup_path(records[n], base_root) for n in BOOTSTRAP_FILES}
                if all(b is not None for b in verified_bkps.values()):
                    pending = _pending_files(win64, ledger)
                    batch = stage_files_for_removal(pending, stage_root, "wwmi-dx12-abort-upgrade") if pending else None
                    for n in BOOTSTRAP_FILES:
                        rb_tmp = win64 / f".wwmi-restore-{uuid.uuid4().hex[:12]}-{n}.tmp"
                        _write_recovery_file(rb_tmp, verified_bkps[n].read_bytes())  # type: ignore[union-attr]
                        os.replace(rb_tmp, win64 / n)
                    ledger.pop("pending_id", None)
                    ledger.update(
                        state="uninstalled",
                        uninstalled_at=_dt.datetime.now(_dt.timezone.utc).isoformat(),
                        restored_previous_owner=prev_owner,
                        staged_batch=str(batch) if batch else None,
                    )
                    _write_json(ledger_path, ledger)
                    return {
                        "state": "uninstalled",
                        "restored_previous_owner": prev_owner,
                        "staged_count": len(pending),
                        "staged_batch": ledger["staged_batch"],
                        "win64_dir": ledger["win64_dir"],
                        "has_legacy_settings": bool(ledger.get("extra_managed_files")),
                    }

            batch = None
            if to_remove:
                batch = stage_files_for_removal(to_remove, stage_root, "wwmi-dx12-uninstall")
            ledger.pop("pending_id", None)
            ledger.update(state="uninstalled", uninstalled_at=_dt.datetime.now(_dt.timezone.utc).isoformat(),
                          staged_batch=str(batch) if batch else None)
            _write_json(ledger_path, ledger)
            return {
                "state": "uninstalled", "staged_count": len(to_remove), "staged_batch": ledger["staged_batch"],
                "win64_dir": ledger["win64_dir"], "has_legacy_settings": bool(ledger.get("extra_managed_files")),
            }
