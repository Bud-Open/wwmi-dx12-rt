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
import uuid
from dataclasses import dataclass, field
from pathlib import Path, PurePath
from typing import Sequence

STEAM_APPID = "3513350"
TARGET_SHIPPING_EXE = "Client-Win64-Shipping.exe"
BOOTSTRAP_FILES = ("dxgi.dll", "ReShade.ini")
ACTIVE_STATES = ("installed", "installing")
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

    @property
    def healthy(self) -> bool:
        return not any(f.level == "error" for f in self.findings)


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
    watch_bytes = {w.encode("utf-8").lower(): w for w in WATCH_PROCESS_NAMES}
    for pid in proc_root.glob("[0-9]*"):
        try:
            argv0 = (pid / "cmdline").read_bytes().split(b"\0", 1)[0].split(b"\n", 1)[0]
        except OSError:
            continue
        base = argv0.replace(b"\\", b"/").rsplit(b"/", 1)[-1].lower()
        if base in watch_bytes:
            hits.append(f"{pid.name}:{watch_bytes[base]}")
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
                    if key == "previous_sha256" and (ledger["state"] != "installing" or key not in item):
                        continue
                    if not re.fullmatch(r"[0-9a-f]{64}", item[key]):
                        raise ValueError(f"{name} 的 {key} 无效")
            if ledger["state"] == "installing" and not re.fullmatch(r"[0-9a-f]{32}", ledger["pending_id"]):
                raise ValueError("安装事务标识无效")
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


def _owned_bootstrap(win64_dir: Path, ledger: dict | None) -> dict[str, str]:
    records = {item["relative"]: item for item in ledger["win64_files"]} if ledger else {}
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
        previous = rec.get("previous_sha256") if ledger["state"] == "installing" else None
        if sha not in (rec["sha256"], previous):
            raise RuntimeError(
                tr(
                    f"Win64/{name} 哈希与账本不一致，拒绝修改",
                    f"Win64/{name} hash does not match the install ledger; refusing to modify.",
                )
            )
        owned[name] = sha
    return owned


def _classify_mount(win64_dir: Path, ledger: dict | None) -> tuple[str, str, dict[str, str]]:
    active = ledger if ledger and ledger["state"] in ACTIVE_STATES else None
    if active:
        recorded = Path(active["win64_dir"])
        if recorded != win64_dir:
            return (
                "active_elsewhere",
                tr(
                    f"账本仍绑定 {recorded}；请先卸载旧游戏目录，再安装到新目录",
                    f"Ledger is still bound to {recorded}; uninstall the previous game directory first.",
                ),
                {},
            )
    owned = _owned_bootstrap(win64_dir, active)
    if not active:
        return "not_installed", tr("Win64 未挂载引导文件", "Not mounted in Win64"), owned
    if active["state"] == "installing":
        return (
            "installing",
            tr(
                "上次安装未完成；可在同一目录重试安装或执行卸载",
                "Previous installation was interrupted; retry Install in the same directory or run Uninstall",
            ),
            owned,
        )
    if len(owned) != len(BOOTSTRAP_FILES):
        return (
            "drifted",
            tr(
                "账本引导文件有缺失，其余文件归属已确认；可重新安装恢复",
                "Some managed bootstrap files are missing (remaining files verified); re-run Install to repair",
            ),
            owned,
        )
    return "installed", tr("已正常挂载 2 文件 BasePath 外部引导桥", "Active (2-file external BasePath mount)"), owned


def inspect_mount_state(win64_dir: Path, base_root: Path) -> tuple[str, str]:
    try:
        state, message, _ = _classify_mount(win64_dir.resolve(), load_install_ledger(base_root))
        return state, message
    except (OSError, RuntimeError) as exc:
        return "conflict", str(exc)


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
) -> DoctorReport:
    p_kind = platform_kind or ("windows" if sys.platform == "win32" else "linux-proton")
    findings: list[DoctorFinding] = []

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

    mount_state, mount_msg = inspect_mount_state(win64_dir, base_root)
    lvl = "ok" if mount_state in ("installed", "not_installed") else ("warn" if mount_state == "drifted" else "error")
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
            wwmi_res = wwmi_dir.resolve()
            in_sandbox = wwmi_res.is_relative_to(fp_root) or (
                compatdata_dir is not None and wwmi_res.is_relative_to(compatdata_dir.resolve())
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


@contextlib.contextmanager
def _mount_lock(base_root: Path):
    path = base_root / "runtime/.mount-lock"
    path.parent.mkdir(parents=True, exist_ok=True)
    _regular_file(path)
    with path.open("a+b") as lock:
        try:
            if sys.platform == "win32":
                import msvcrt
                lock.seek(0)
                msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as exc:
            raise RuntimeError(f"无法锁定安装账本，可能有另一安装/卸载正在运行：{path}") from exc
        yield  # Closing the file releases the OS lock, including after a crash.


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
    return b"\xff\xfe" + text.replace("\n", "\r\n").encode("utf-16-le")


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
    for relative in ("runtime", "runtime/logs", "runtime/cache/shaders", "runtime/seed",
                     "host", "reshade-addons/wwmi_dx12/dxc", "reshade-addons/rt-pack-data", "seed", "lib"):
        directory = base_root / relative
        actual = directory.resolve()
        if not actual.is_relative_to(base_root) or actual.is_relative_to(wwmi_dir):
            raise ValueError(f"拒绝通过外部运行时目录写入其他目录：{directory}")
    if check_processes and (running := running_watched_processes()):
        raise RuntimeError(f"检测到游戏正在运行（{', '.join(running)}），请先正常退出游戏")


def _pending_files(win64_dir: Path, ledger: dict | None) -> list[Path]:
    if not ledger or ledger["state"] != "installing":
        return []
    paths = [win64_dir / f".wwmi-{ledger['pending_id']}-{name}.tmp" for name in BOOTSTRAP_FILES]
    return [path for path in paths if _regular_file(path)]


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
) -> dict:
    """Sync the payload and commit the bootstrap. compatdata_dir is no longer modified."""
    p_kind = platform_kind or ("windows" if sys.platform == "win32" else "linux-proton")
    supplied_win64 = win64_dir.absolute()
    win64_dir = win64_dir.resolve()
    wwmi_dir = wwmi_dir.resolve()
    base_root = base_root.resolve()
    _check_install_target(win64_dir, base_root, wwmi_dir, check_processes)
    if supplied_win64 != win64_dir:
        _check_install_target(supplied_win64, base_root, wwmi_dir, False)
    if not (wwmi_dir / "d3dx.ini").is_file() and not init_missing_wwmi:
        raise ValueError(f"无效的 WWMI 目录（缺少 d3dx.ini）：{wwmi_dir}")

    def preflight() -> tuple[dict | None, dict[str, str]]:
        ledger = load_install_ledger(base_root)
        state, msg, owned = _classify_mount(win64_dir, ledger)
        if state == "active_elsewhere":
            raise RuntimeError(msg)
        return ledger, owned

    preflight()
    payload_map = resolve_payload_sources(source_root.resolve())
    engine_base = format_engine_path(base_root, p_kind)
    engine_wwmi = format_engine_path(wwmi_dir, p_kind)
    engine_runtime = format_engine_path(base_root / "runtime", p_kind)
    if "\n" in engine_wwmi or "\r" in engine_wwmi:
        raise ValueError("WWMI 路径不得含换行符")
    stub_data = build_win64_stub_reshade_ini(engine_base).encode("utf-8")
    if dry_run:
        options, warning = proton_launch_options(base_root, proton_utf8_guard) if p_kind == "linux-proton" else (None, None)
        return {
            "mode": "dry-run", "platform_kind": p_kind, "base_root": str(base_root),
            "win64_dir": str(win64_dir), "wwmi_dir": str(wwmi_dir),
            "engine_base_path": engine_base, "engine_wwmi_dir": engine_wwmi,
            "win64_bootstrap_files": [str(win64_dir / name) for name in BOOTSTRAP_FILES],
            "external_payload_count": len(payload_map),
            "proton_launch_options": options, "proton_preload_warning": warning,
        }

    stage_root = staging_root or default_staging_root(base_root)
    with _mount_lock(base_root):
        _check_install_target(win64_dir, base_root, wwmi_dir, check_processes)
        old_ledger, owned = preflight()
        abandoned = _pending_files(win64_dir, old_ledger)
        if abandoned:
            stage_files_for_removal(abandoned, stage_root, "wwmi-dx12-interrupted-install")

        dxgi_data = payload_map["host/ReShade64-game-forward.dll"].read_bytes()
        bootstrap = dict(zip(BOOTSTRAP_FILES, (dxgi_data, stub_data)))
        pending_id = uuid.uuid4().hex
        ledger = {
            "schema_version": 1, "state": "installing", "pending_id": pending_id,
            "platform_kind": p_kind, "base_root": str(base_root),
            "win64_dir": str(win64_dir), "wwmi_dir": str(wwmi_dir),
            "win64_files": [
                {"relative": name, "path": str(win64_dir / name), "sha256": hashlib.sha256(data).hexdigest(),
                 "size": len(data), **({"previous_sha256": owned[name]} if name in owned else {})}
                for name, data in bootstrap.items()
            ],
            # Older releases wrote settings in compatdata. Keep the record for recovery,
            # but never move or modify out-of-scope paths supplied by the ledger.
            "extra_managed_files": old_ledger.get("extra_managed_files", []) if old_ledger else [],
        }
        ledger_path = _ledger_path(base_root)
        _write_json(ledger_path, ledger)
        if init_missing_wwmi and not (wwmi_dir / "d3dx.ini").is_file():
            init_wwmi_skeleton(wwmi_dir)
        for rel, src in payload_map.items():
            dst = base_root / rel
            dst.parent.mkdir(parents=True, exist_ok=True)
            if dst != src:
                _atomic_write(dst, dxgi_data if rel == "host/ReShade64-game-forward.dll" else src.read_bytes())
        for sub in ("runtime/logs", "runtime/cache/shaders", "runtime/seed", "seed"):
            (base_root / sub).mkdir(parents=True, exist_ok=True)
        ext_reshade_path = base_root / "ReShade.ini"
        existing_reshade = ext_reshade_path.read_text(encoding="utf-8", errors="ignore") if _regular_file(ext_reshade_path) else None
        _atomic_write(ext_reshade_path, merge_external_reshade_ini(existing_reshade).encode("utf-8"))
        _atomic_write(base_root / "wwmi_dx12.ini", encode_wwmi_dx12_ini(
            build_wwmi_dx12_ini(engine_wwmi, engine_runtime, skin_mode=skin_mode)
        ))

        if check_processes and (running := running_watched_processes()):
            raise RuntimeError(f"检测到游戏正在运行（{', '.join(running)}），请退出后重试安装")
        owned = _owned_bootstrap(win64_dir, ledger)
        if owned:
            stage_files_for_removal([win64_dir / name for name in owned], stage_root, "wwmi-dx12-reinstall")
        for name, data in bootstrap.items():
            dst = win64_dir / name
            temp = win64_dir / f".wwmi-{pending_id}-{name}.tmp"
            _write_new_file(temp, data)
            if os.path.lexists(dst):
                raise RuntimeError(f"安装期间已有其他程序创建 {dst}，拒绝覆盖；可在同一目录重试")
            if sys.platform == "win32":
                temp.rename(dst)  # Windows rename refuses an existing destination.
            else:
                os.replace(temp, dst)
        ledger.pop("pending_id")
        for rec in ledger["win64_files"]:
            rec.pop("previous_sha256", None)
        ledger.update(state="installed", installed_at=_dt.datetime.now(_dt.timezone.utc).isoformat())
        _write_json(ledger_path, ledger)
        if p_kind == "linux-proton":
            ledger["proton_launch_options"], ledger["proton_preload_warning"] = proton_launch_options(
                base_root, proton_utf8_guard,
            )
        return ledger


def uninstall_mount(
    base_root: Path,
    dry_run: bool = False,
    staging_root: Path | None = None,
    check_processes: bool = True,
) -> dict:
    """Stage only bootstrap files with a verified name, location and digest."""
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
        return [win64 / name for name in _owned_bootstrap(win64, ledger)] + _pending_files(win64, ledger)

    ledger = load_install_ledger(base_root)
    if not ledger or ledger["state"] not in ACTIVE_STATES:
        return noop
    if dry_run:
        return {"state": "dry-run", "will_stage": [str(p) for p in preview(ledger)]}

    with _mount_lock(base_root):
        ledger = load_install_ledger(base_root)
        if not ledger or ledger["state"] not in ACTIVE_STATES:
            return noop
        to_remove = preview(ledger)
        batch = None
        if to_remove:
            batch = stage_files_for_removal(
                to_remove, staging_root or default_staging_root(base_root), "wwmi-dx12-uninstall",
            )
        ledger.update(state="uninstalled", uninstalled_at=_dt.datetime.now(_dt.timezone.utc).isoformat(),
                      staged_batch=str(batch) if batch else None)
        _write_json(ledger_path, ledger)
        return {
            "state": "uninstalled", "staged_count": len(to_remove), "staged_batch": ledger["staged_batch"],
            "win64_dir": ledger["win64_dir"], "has_legacy_settings": bool(ledger.get("extra_managed_files")),
        }
