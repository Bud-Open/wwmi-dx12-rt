#!/usr/bin/env python3
"""Stage5 临时注入：按所选档向鸣潮 EXE 目录新增精确清单（绝不覆盖）。

硬约束（写在代码里，不靠文档自觉）：
  * 只写 Client/Binaries/Win64 下所选档列出的文件和目录；
  * 任一目标已存在即整体拒绝，不覆盖、不合并、不备份改名；
  * 二进制用 open('xb') 直接落到目标，不造临时文件，不删除任何东西；
  * 从第一步就写 deploy-manifest.json，失败也保留已创建清单；
  * 不碰 Steam/XXMI 启动项、注册表、DLL override、存档、画质配置。

用法：
  python3 scripts/wwmi-stage5-native-host-deploy.py \
    --package <解压包目录> --results <结果目录> [--dry-run]
  python3 scripts/wwmi-stage5-native-host-deploy.py --self-test

--dry-run 只做前置检查与打印，不建目录、不写文件。
"""
from __future__ import annotations

import argparse
import json
import sys
from datetime import datetime, timezone
from pathlib import Path

from wwmi_rt_extra_data import RT_PACK_SHADERS

GAME_WIN64 = Path.home() / ".local/share/Steam/steamapps/common/Wuthering Waves/Client/Binaries/Win64"
# /proc/comm 会截断到 15 字节，因此一律比对 cmdline argv[0] 的 basename。
WATCH_EXECUTABLES = (
    "Client-Win64-Shipping.exe",
    "Wuthering Waves.exe",
    "XXMI Launcher.exe",
)

# (目标相对路径, 包内来源；None 表示脚本按 profile 生成)
OBSERVER_PLAN = (
    ("dxgi.dll", "artifacts/stage5/ReShade64-binding-trace.dll"),
    ("ReShade.ini", None),
    ("reshade-addons/WWMI_Observer.addon64", "artifacts/stage5/WWMI_Observer_Stage5.addon64"),
    ("reshade-addons/WWMI_Observer.ini", None),
)

# 候选档：不装观察器，避免与宿主自带的 binding 桥来源混在一起。
CANDIDATE_PLAN = (
    ('dxgi.dll', 'artifacts/stage5/ReShade64-game-forward.dll'),
    ('ReShade.ini', None),
    ('reshade-addons/wwmi_dx12.addon64', 'artifacts/stage5/wwmi_dx12.addon64'),
    ('reshade-addons/zz_wwmi_rt_pack.addon64', 'artifacts/stage5/zz_wwmi_rt_pack.addon64'),
    ('reshade-addons/wwmi_dx12/Bc7Half.cso', 'artifacts/stage5/wwmi_dx12/Bc7Half.cso'),
    ('reshade-addons/wwmi_dx12/BufCopy.cso', 'artifacts/stage5/wwmi_dx12/BufCopy.cso'),
    ('reshade-addons/wwmi_dx12/IbVbHash.cso', 'artifacts/stage5/wwmi_dx12/IbVbHash.cso'),
    ('wwmi_dx12.ini', None),
)

# Package-specific additions use the same ledger as the base payload.
CANDIDATE_PLAN += tuple(
    (f"reshade-addons/rt-pack-data/{name}", f"artifacts/stage5/rt-pack-data/{name}")
    for name in RT_PACK_SHADERS
)

# RabbitFX 与程序契约只读反汇编复用同一 DXC 连接。DLL 必须与候选一起落地，
# 不能依赖游戏、原版 WWMI 树或前一次会话的副本。
CANDIDATE_PLAN += (
    ("reshade-addons/wwmi_dx12/dxc/dxcompiler.dll", "artifacts/stage5/wwmi_dx12/dxc/dxcompiler.dll"),
    ("reshade-addons/wwmi_dx12/dxc/dxil.dll", "artifacts/stage5/wwmi_dx12/dxc/dxil.dll"),
)

PROFILES = {"observer": OBSERVER_PLAN, "candidate": CANDIDATE_PLAN}

# 需要整体新建的目录（相对 EXE 目录）
PROFILE_DIRS = {
    "observer": ("reshade-addons",),
    "candidate": ("reshade-addons", "reshade-addons/wwmi_dx12", "reshade-addons/rt-pack-data",
                  "reshade-addons/wwmi_dx12/dxc"),
}

# 运行期产物：只在部署前不存在时才计入回滚
RUNTIME_WATCH = {
    "observer": ("ReShade.log", *(f"ReShade.log{i}" for i in range(1, 11)), "ReShadePreset.ini"),
    "candidate": (
        "ReShade.log",
        *(f"ReShade.log{i}" for i in range(1, 11)),
        "ReShadePreset.ini",
        "wwmi_dx12_addon.log",
        "wwmi_dx12_addon.log.1",
        # addon 每次加载都会把上一份日志改名成 .prev.log（addon64 内相邻字面量
        # ".prev.log" 与 "wwmi_dx12_addon.log"）。此前漏记，导致 0 字节残留留在
        # 游戏目录、recover 按"账本外不碰"拒绝收走。
        "wwmi_dx12_addon.prev.log",
        # cx058 RFX 探针（候选补丁 0001-cx058-rfx-skeleton-probe.patch）的输出。
        # 探针以追加模式写在 exe 同目录；不登记会重演 .prev.log 被"账本外不碰"
        # 拒收的同类遗漏，残留会留在游戏目录里。
        "wwmi_dx12_cx058_probe.log",
    ),
}

# 运行期目录：候选会在 EXE 旁建 wwmi_dx12\cache\shaders
RUNTIME_DIR_WATCH = {"observer": (), "candidate": ("wwmi_dx12",)}

RESHADE_INI = """\
[INSTALL]
Logging=1

[ADDON]
AddonPath=reshade-addons

[OVERLAY]
TutorialProgress=4
"""


def candidate_ini(windows_wwmi_dir: str, uav_observe: bool = False) -> str:
    """生成隔离树路径；仅本次显式启用时增加被动 UAV 探针开关。

    指到游戏目录之外的隔离树，d3dx_user.ini / DX12_Dumps / 缓存写回都落在那里，
    不会碰到用户已验收的 XXMI 原树。
    """
    return (
        "; Stage5 DX12 candidate: isolated WWMI tree, never the live XXMI install.\n"
        "[wwmi-dx12]\n"
        f"wwmi_dir={windows_wwmi_dir}\n"
        + ("uav_observe=1\n" if uav_observe else "")
    )

# 自检用的故障注入点；正常运行时为 None。
_POST_CREATE_HOOK = None


def observer_ini(windows_dir: str) -> str:
    """duration_seconds 是录制上限，不是测试时长：观察器从首个 D3D12 设备创建时开始计时，
    包里没有场景触发开关，合法区间 1-3600，不能写 0。"""
    return (
        "[observer]\n"
        "; Observation only. This does not enable/disable any game renderer or Mod.\n"
        "enabled=1\n"
        "draw_every=128\n"
        "dispatch_every=128\n"
        "duration_seconds=1800\n"
        "max_file_mib=256\n"
        "; Explicit drive path; the add-on creates only the last component.\n"
        f"output_directory={windows_dir}\n"
    )


def windows_dir_for(posix: Path) -> str:
    """Proton 前缀内 Z: 映射宿主根；观察器要求尾部反斜杠。"""
    return "Z:" + str(posix).replace("/", "\\") + "\\"


def windows_wwmi_dir_for(posix: Path) -> str:
    """wwmi_dir 会原样交给 Win32 路径 API；不带尾部反斜杠反而更安全。"""
    return "Z:" + str(posix).replace("/", "\\")


def pid_namespace_scoped() -> bool:
    """沙箱/容器里 /proc 只含本命名空间的进程，宿主进程不可见。

    pid 1 不是 init/systemd 即视为命名空间隔离；此时「没检测到」不能等同于「没运行」。
    """
    try:
        name = Path("/proc/1/comm").read_text(encoding="utf-8", errors="replace").strip()
    except OSError:
        return True
    return name not in {"init", "systemd"}


def process_scan() -> dict:
    """按 cmdline argv[0] 的 basename 找游戏/XXMI，并如实报告可见性。

    /proc/comm 截断到 15 字节，用它比较完整 exe 名会漏报；改用 cmdline。
    读不到 cmdline 的进程计入 unreadable；PID 命名空间隔离时 visibility 记
    namespace_scoped，绝不据此声称「都没有运行」。
    """
    watched = {name.lower() for name in WATCH_EXECUTABLES}
    readable = 0
    unreadable = 0
    detected = []
    for entry in Path("/proc").iterdir():
        if not entry.name.isdigit():
            continue
        try:
            raw = (entry / "cmdline").read_bytes()
        except OSError:
            unreadable += 1
            continue
        readable += 1
        if not raw:
            continue  # 内核线程，无 argv
        argv0 = raw.split(b"\0", 1)[0].decode("utf-8", "replace")
        basename = argv0.replace("\\", "/").rsplit("/", 1)[-1].strip()
        if basename.lower() in watched:
            detected.append({"pid": int(entry.name), "argv0": argv0})
    namespace_scoped = pid_namespace_scoped()
    return {
        "cmdline_readable": readable,
        "cmdline_unreadable": unreadable,
        "namespace_scoped": namespace_scoped,
        "detection_complete": unreadable == 0 and not namespace_scoped,
        "visible_pid_count": readable + unreadable,
        "detected": detected,
    }


def snapshot_runtime(win64: Path, watch) -> dict:
    """部署前记录拟跟踪的 ReShade/候选产物是否已存在；用于判断回滚归属。"""
    snapshot = {}
    for relative in watch:
        path = win64 / relative
        exists = path.exists() or path.is_symlink()
        snapshot[relative] = {
            "exists": exists,
            "size": path.stat().st_size if exists and path.is_file() else None,
        }
    return snapshot


def write_manifest(path: Path, record: dict) -> None:
    """写自己的账本文件（覆盖自身，不删除任何他人文件）。"""
    with path.open("w", encoding="utf-8") as handle:
        json.dump(record, handle, ensure_ascii=False, indent=2)
        handle.write("\n")


def check_absent(path: Path) -> None:
    if path.exists() or path.is_symlink():
        raise SystemExit(f"拒绝部署：目标已存在 {path}")


def prepare_results(results: Path) -> None:
    if results.exists():
        if not results.is_dir():
            raise SystemExit(f"拒绝部署：结果路径不是目录 {results}")
        if any(results.iterdir()):
            raise SystemExit(f"拒绝部署：结果目录非空 {results}")
    else:
        results.mkdir(parents=True)


def copy_exclusive(source: Path, target: Path) -> int:
    """open('xb') 直接创建目标；不做临时文件、不做 rename、不删除。"""
    if not source.is_file():
        raise SystemExit(f"拒绝部署：包内来源缺失 {source}")
    if target.exists() or target.is_symlink():
        raise SystemExit(f"拒绝部署：目标已存在 {target}")
    written = 0
    with source.open("rb") as src, target.open("xb") as dst:
        while True:
            chunk = src.read(1 << 20)
            if not chunk:
                break
            dst.write(chunk)
            written += len(chunk)
        dst.flush()
    return written


def write_text_exclusive(target: Path, text: str, *, utf16le: bool = False) -> int:
    if target.exists() or target.is_symlink():
        raise SystemExit(f"拒绝部署：目标已存在 {target}")
    data = b"\xff\xfe" + text.replace("\n", "\r\n").encode("utf-16-le") if utf16le else text.encode("ascii")
    with target.open("xb") as handle:
        handle.write(data)
        handle.flush()
    return len(data)


def deploy(package: Path, results: Path, dry_run: bool, win64: Path | None = None,
           profile: str = "observer", wwmi_tree: Path | None = None,
           uav_observe: bool = False) -> int:
    if uav_observe and profile != "candidate":
        raise SystemExit("--uav-observe 仅支持 candidate 档")
    win64 = win64 or GAME_WIN64
    plan = PROFILES[profile]
    plan_dirs = PROFILE_DIRS[profile]
    runtime_watch = RUNTIME_WATCH[profile]
    runtime_dirs = RUNTIME_DIR_WATCH[profile]

    if not win64.is_dir():
        raise SystemExit(f"拒绝部署：游戏目录不存在 {win64}")
    if not package.is_dir():
        raise SystemExit(f"拒绝部署：包目录不存在 {package}")
    if profile == "candidate":
        if wwmi_tree is None:
            raise SystemExit("拒绝部署：candidate 档必须给 --wwmi-tree（游戏目录外的隔离树）")
        if not (wwmi_tree / "d3dx.ini").is_file():
            raise SystemExit(f"拒绝部署：隔离树缺少 d3dx.ini {wwmi_tree}")
        if not (wwmi_tree / "Mods").is_dir():
            raise SystemExit(f"拒绝部署：隔离树缺少 Mods/ {wwmi_tree}")
        if str(wwmi_tree.resolve()).startswith(str(win64.resolve())):
            raise SystemExit("拒绝部署：隔离树不能放在游戏目录内")

    scan = process_scan()
    if scan["detected"]:
        raise SystemExit(
            "拒绝部署：检测到游戏/XXMI 进程 " + json.dumps(scan["detected"], ensure_ascii=False)
        )

    for relative, _ in plan:
        check_absent(win64 / relative)
    for relative in plan_dirs:
        check_absent(win64 / relative)

    sources = {}
    for relative, origin in plan:
        if origin is None:
            continue
        source = package / origin
        if not source.is_file():
            raise SystemExit(f"拒绝部署：包内来源缺失 {source}")
        sources[relative] = source

    snapshot = snapshot_runtime(win64, runtime_watch)
    pre_existing = [rel for rel, state in snapshot.items() if state["exists"]]
    dir_snapshot = {rel: (win64 / rel).exists() for rel in runtime_dirs}
    pre_existing_dirs = [rel for rel, exists in dir_snapshot.items() if exists]

    ini_dir = windows_dir_for(results)
    if profile == "candidate":
        wwmi_win = windows_wwmi_dir_for(wwmi_tree)
        if len(wwmi_win) >= 259:
            raise SystemExit("拒绝部署：wwmi_dir 超过 MAX_PATH")

    if dry_run:
        print(f"[dry-run] 档位：{profile}")
        print(f"[dry-run] 游戏目录：{win64}")
        print(f"[dry-run] 结果目录：{results}")
        for relative in plan_dirs:
            print(f"[dry-run] 将新建目录 {win64 / relative}")
        for relative, _ in plan:
            print(f"[dry-run] 将新增 {win64 / relative}")
        if profile == "observer":
            print(f"[dry-run] output_directory={ini_dir}")
        else:
            print(f"[dry-run] 隔离树：{wwmi_tree}")
            print(f"[dry-run] wwmi_dir={wwmi_win}")
            if uav_observe:
                print("[dry-run] uav_observe=1（本次会话）")
        print(
            f"[dry-run] 进程扫描：可读 {scan['cmdline_readable']}，"
            f"不可读 {scan['cmdline_unreadable']}，命中 {len(scan['detected'])}"
        )
        if scan["namespace_scoped"]:
            print(
                "[dry-run] 注意：当前 /proc 处于独立 PID 命名空间，"
                f"只能看到 {scan['visible_pid_count']} 个进程；"
                "宿主上的游戏/XXMI 不可见，不能据此断言都没运行"
            )
        elif not scan["detection_complete"]:
            print("[dry-run] 注意：有进程 cmdline 不可读，无法据此断言「都没有运行」")
        print(f"[dry-run] 部署前已存在的运行产物：{pre_existing or '无'}")
        print(f"[dry-run] 部署前已存在的运行目录：{pre_existing_dirs or '无'}")
        return 0

    prepare_results(results)
    manifest_path = results / "deploy-manifest.json"
    now = lambda: datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
    record = {
        "state": "prepared",
        "profile": profile,
        "game_win64": str(win64),
        "package": str(package),
        "results": str(results),
        "observer_output_directory": ini_dir if profile == "observer" else None,
        "wwmi_tree": str(wwmi_tree) if wwmi_tree else None,
        "wwmi_dir_windows": wwmi_win if profile == "candidate" else None,
        "steam_or_xxmi_launch_options_modified": False,
        "registry_or_dll_override_added": False,
        "process_scan": scan,
        "runtime_snapshot": snapshot,
        "pre_existing_runtime_artifacts": pre_existing,
        "runtime_watch": list(runtime_watch),
        "runtime_dir_watch": list(runtime_dirs),
        "pre_existing_runtime_dirs": pre_existing_dirs,
        "created_directories": [
            {"relative": rel, "path": str(win64 / rel), "created": False, "created_utc": None}
            for rel in plan_dirs
        ],
        "files": [
            {
                "relative": relative,
                "path": str(win64 / relative),
                "source": origin,
                "created": False,
                "size": None,
                "created_utc": None,
            }
            for relative, origin in plan
        ],
        "started_utc": now(),
    }
    if uav_observe:
        record["uav_observe"] = True
    write_manifest(manifest_path, record)

    try:
        for entry in record["created_directories"]:
            target = win64 / entry["relative"]
            target.mkdir()
            entry.update(created=True, created_utc=now())
            write_manifest(manifest_path, record)
        if _POST_CREATE_HOOK is not None:
            _POST_CREATE_HOOK()

        for entry in record["files"]:
            relative = entry["relative"]
            target = win64 / relative
            if relative == "ReShade.ini":
                size = write_text_exclusive(target, RESHADE_INI)
            elif relative == "reshade-addons/WWMI_Observer.ini":
                size = write_text_exclusive(target, observer_ini(ini_dir))
            elif relative == "wwmi_dx12.ini":
                size = write_text_exclusive(target, candidate_ini(wwmi_win, uav_observe), utf16le=True)
            else:
                size = copy_exclusive(sources[relative], target)
            entry.update(created=True, size=size, created_utc=now())
            write_manifest(manifest_path, record)

        record["state"] = "deployed"
        record["finished_utc"] = now()
        write_manifest(manifest_path, record)
    except BaseException as exc:
        # 拷贝中断时目标可能已落地但尚未标 created（例如只写了一半）。
        # 不删除半成品，改为按本轮已知路径核对磁盘实况并记账，交给 recover 收走。
        for entry in record["files"]:
            target = win64 / entry["relative"]
            if not entry["created"] and (target.exists() or target.is_symlink()):
                entry["created"] = True
                entry["partial"] = True
                entry["size"] = target.stat().st_size if target.is_file() else None
                entry["created_utc"] = now()
        for entry in record["created_directories"]:
            target = win64 / entry["relative"]
            if not entry["created"] and target.is_dir():
                entry.update(created=True, created_utc=now(), partial=True)
        record["state"] = "failed"
        record["error"] = f"{type(exc).__name__}: {exc}"
        record["finished_utc"] = now()
        write_manifest(manifest_path, record)
        print("部署中断，账本已保留，未执行任何删除：", file=sys.stderr)
        for entry in record["created_directories"]:
            if entry["created"]:
                print(f"  已创建目录 {entry['path']}", file=sys.stderr)
        for entry in record["files"]:
            if entry["created"]:
                mark = "（半成品，需按账本收回）" if entry.get("partial") else ""
                print(f"  已创建 {entry['path']}{mark}", file=sys.stderr)
        print(f"  账本 {manifest_path}", file=sys.stderr)
        print(
            "用 wwmi-stage5-native-host-recover.py 按该账本暂存已创建对象。", file=sys.stderr
        )
        raise

    print(f"已部署 {profile} 档 {len(plan)} 个新路径到 {win64}")
    print(f"结果目录 {results}")
    print(f"部署记录 {manifest_path}")
    if profile == "candidate":
        print(f"隔离树 {wwmi_tree}（wwmi_dir={wwmi_win}）")
    if pre_existing:
        print(f"部署前已存在的运行产物（不计入本轮回滚）：{pre_existing}")
    if pre_existing_dirs:
        print(f"部署前已存在的运行目录（不计入本轮）：{pre_existing_dirs}")
    if scan["namespace_scoped"]:
        print(
            f"注意：当前 PID 命名空间隔离，只可见 {scan['visible_pid_count']} 个进程；"
            "宿主上的游戏/XXMI 状态需由执行侧另行确认"
        )
    elif not scan["detection_complete"]:
        print("注意：部分进程 cmdline 不可读，未运行状态不完整")
    return 0


def addon_directory_path(record: dict) -> str:
    return record["addon_directory"]["path"]


def self_test() -> int:
    """测试物放 mkdtemp 并保留，不自动清理。"""
    root = Path(__import__("tempfile").mkdtemp(prefix="wwmi-deploy-selftest-"))
    win = root / "Win64"
    win.mkdir()
    package = root / "pkg"
    for _, source in (*OBSERVER_PLAN, *CANDIDATE_PLAN):
        if source:
            payload = package / source
            payload.parent.mkdir(parents=True, exist_ok=True)
            payload.write_bytes(b"test-payload")

    tree = root / "中文隔离树"
    (tree / "Mods/Example/Example Skin").mkdir(parents=True)
    (tree / "d3dx.ini").write_text("[Include]\n")

    assert windows_dir_for(root / "obs") == "Z:" + str(root / "obs").replace("/", "\\") + "\\"
    assert windows_wwmi_dir_for(tree) == "Z:" + str(tree).replace("/", "\\")
    assert not windows_wwmi_dir_for(tree).endswith("\\")
    assert RESHADE_INI.isascii()
    assert "[ADDON]" in RESHADE_INI and "AddonPath=reshade-addons" in RESHADE_INI
    ini = candidate_ini(windows_wwmi_dir_for(tree))
    assert "[wwmi-dx12]" in ini
    assert ini.split("wwmi_dir=")[1].strip() == windows_wwmi_dir_for(tree)

    # 1) observer 档照旧工作。
    obs_results = root / "obs"
    assert deploy(package, obs_results, False, win64=win, profile="observer") == 0
    rec = json.loads((obs_results / "deploy-manifest.json").read_text())
    assert rec["profile"] == "observer" and rec["state"] == "deployed"
    assert len(rec["files"]) == 4 and all(f["created"] for f in rec["files"])
    assert (win / "ReShade.ini").read_text() == RESHADE_INI
    assert (win / "reshade-addons/WWMI_Observer.addon64").exists()

    # 2) candidate 档必须完整部署，Unicode INI 指向隔离树。
    win2 = root / "Win64-cand"
    win2.mkdir()
    cand_results = root / "cand-obs"
    assert deploy(package, cand_results, False, win64=win2, profile="candidate",
                  wwmi_tree=tree) == 0
    rec2 = json.loads((cand_results / "deploy-manifest.json").read_text())
    assert rec2["profile"] == "candidate" and rec2["state"] == "deployed"
    assert len(rec2["files"]) == len(CANDIDATE_PLAN), [f["relative"] for f in rec2["files"]]
    rels = {f["relative"] for f in rec2["files"]}
    assert "ReShade.ini" in rels, "candidate 档必须包含 ReShade.ini"
    assert "reshade-addons/wwmi_dx12/Bc7Half.cso" in rels
    assert (win2 / "ReShade.ini").read_text() == RESHADE_INI
    assert (win2 / "reshade-addons/wwmi_dx12.addon64").read_bytes() == b"test-payload"
    cand_ini = (win2 / "wwmi_dx12.ini").read_text(encoding="utf-16")
    assert windows_wwmi_dir_for(tree) in cand_ini
    assert tuple(d["relative"] for d in rec2["created_directories"]) == PROFILE_DIRS["candidate"]
    assert all(d["created"] for d in rec2["created_directories"])
    assert "wwmi_dx12_addon.log" in rec2["runtime_watch"]
    assert "wwmi_dx12_addon.prev.log" in rec2["runtime_watch"]
    assert rec2["runtime_dir_watch"] == ["wwmi_dx12"]
    assert rec2["pre_existing_runtime_artifacts"] == []
    assert rec2["pre_existing_runtime_dirs"] == []

    # 3) candidate 档缺 --wwmi-tree 必须拒绝，且不写任何东西。
    win3 = root / "Win64-noTree"
    win3.mkdir()
    try:
        deploy(package, root / "obs-noTree", False, win64=win3, profile="candidate")
    except SystemExit:
        pass
    else:
        raise AssertionError("candidate 档缺隔离树未被拒绝")
    assert not (win3 / "dxgi.dll").exists()

    # 4) 隔离树没有 d3dx.ini 必须拒绝。
    win4 = root / "Win64-badTree"
    win4.mkdir()
    empty_tree = root / "empty-tree"
    (empty_tree / "Mods").mkdir(parents=True)
    try:
        deploy(package, root / "obs-badTree", False, win64=win4, profile="candidate",
               wwmi_tree=empty_tree)
    except SystemExit:
        pass
    else:
        raise AssertionError("缺 d3dx.ini 的隔离树未被拒绝")
    assert not (win4 / "dxgi.dll").exists()

    # 5) 隔离树放在游戏目录内必须拒绝。
    inside = win / "wwmi-inside"
    (inside / "Mods").mkdir(parents=True)
    (inside / "d3dx.ini").write_text("x")
    win5 = root / "Win64-inside"
    win5.mkdir()
    inside5 = win5 / "tree"
    (inside5 / "Mods").mkdir(parents=True)
    (inside5 / "d3dx.ini").write_text("x")
    try:
        deploy(package, root / "obs-inside", False, win64=win5, profile="candidate",
               wwmi_tree=inside5)
    except SystemExit:
        pass
    else:
        raise AssertionError("隔离树位于游戏目录内未被拒绝")
    assert not (win5 / "dxgi.dll").exists()

    # 6) 已存在的 ReShade.ini 必须整体拒绝（不能覆盖已有文件）。
    win6 = root / "Win64-exists"
    win6.mkdir()
    (win6 / "ReShade.ini").write_text("pre-existing")
    try:
        deploy(package, root / "obs-exists", False, win64=win6, profile="candidate",
               wwmi_tree=tree)
    except SystemExit:
        pass
    else:
        raise AssertionError("已存在 ReShade.ini 未被拒绝")
    assert (win6 / "ReShade.ini").read_text() == "pre-existing"

    # 7) 部署前已存在的 wwmi_dx12 目录与日志必须记为 pre-existing（不计入回滚）。
    win7 = root / "Win64-pre"
    win7.mkdir()
    (win7 / "wwmi_dx12").mkdir()
    (win7 / "wwmi_dx12_addon.log").write_text("old")
    r7 = root / "obs-pre"
    assert deploy(package, r7, False, win64=win7, profile="candidate", wwmi_tree=tree) == 0
    rec7 = json.loads((r7 / "deploy-manifest.json").read_text())
    assert "wwmi_dx12_addon.log" in rec7["pre_existing_runtime_artifacts"]
    assert rec7["pre_existing_runtime_dirs"] == ["wwmi_dx12"]

    # 8) 中途失败：目录已建但文件未落地，账本 state=failed 且保留目录清单。
    win8 = root / "Win64-fail"
    win8.mkdir()
    r8 = root / "obs-fail"

    def boom() -> None:
        (win8 / "dxgi.dll").write_bytes(b"half")
        raise RuntimeError("injected")

    global _POST_CREATE_HOOK
    _POST_CREATE_HOOK = boom
    try:
        try:
            deploy(package, r8, False, win64=win8, profile="candidate", wwmi_tree=tree)
        except RuntimeError:
            pass
        else:
            raise AssertionError("故障注入未生效")
        rec8 = json.loads((r8 / "deploy-manifest.json").read_text())
        assert rec8["state"] == "failed", rec8["state"]
        assert all(d["created"] for d in rec8["created_directories"])
        dxgi = [f for f in rec8["files"] if f["relative"] == "dxgi.dll"][0]
        assert dxgi["created"] is True and dxgi.get("partial") is True, dxgi
    finally:
        _POST_CREATE_HOOK = None

    print(f"self-test ok（测试物保留在 {root}）")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--package")
    parser.add_argument("--results")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--uav-observe", action="store_true", help="仅本次 candidate 会话启用有界 UAV 观察")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--profile", choices=sorted(PROFILES), default="observer")
    parser.add_argument("--wwmi-tree", help="candidate 档必填：游戏目录外的隔离 WWMI 树")
    parser.add_argument("--win64", help="覆盖 EXE 目录（仅用于本地替身测试，默认真实游戏目录）")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not args.package or not args.results:
        parser.error("需要 --package 与 --results（或 --self-test）")
    return deploy(
        Path(args.package),
        Path(args.results),
        args.dry_run,
        profile=args.profile,
        wwmi_tree=Path(args.wwmi_tree) if args.wwmi_tree else None,
        win64=Path(args.win64) if args.win64 else None,
        uav_observe=args.uav_observe,
    )


if __name__ == "__main__":
    raise SystemExit(main())
