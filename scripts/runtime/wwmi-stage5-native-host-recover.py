#!/usr/bin/env python3
"""按本轮 deploy-manifest.json 把注入文件与 ReShade 新增产物暂存到删除暂存区。

归属规则（不靠"目录里有什么"，靠账本）：
  * 只处理账本里 `created=true` 的对象；
  * 运行产物只收回"部署前不存在"的那些（账本 `pre_existing_runtime_artifacts` 排除）；
  * `reshade-addons/` 整目录移入暂存批次，不删空目录；
  * 账本外的文件一律不碰，发现即整体拒绝并列出；
  * trace 目录原位保留，只读取文件名与大小作证据。

只做移动，不删除任何东西：不调用 unlink/rmdir/rmtree，也不清理批次。

用法：
  python3 scripts/wwmi-stage5-native-host-recover.py --results <结果目录> \
    [--batch batch-YYYYmmdd-HHMMSS-wwmi-phaseA] [--dry-run]
  python3 scripts/wwmi-stage5-native-host-recover.py --self-test
"""
from __future__ import annotations

import argparse
import json
import shutil
import sys
import tempfile
from datetime import datetime
from pathlib import Path

from wwmi_rt_extra_data import RT_PACK_SHADERS

STAGING_ROOT = Path.home() / ".pi" / "deletion-staging"
OWNED_FILES = {
    # observer 档
    "dxgi.dll",
    "ReShade.ini",
    "reshade-addons/WWMI_Observer.addon64",
    "reshade-addons/WWMI_Observer.ini",
    # candidate 档
    "reshade-addons/WWMI_DX12_Capture_Stage5.addon64",
    "reshade-addons/wwmi_dx12/Bc7Half.cso",
    "reshade-addons/wwmi_dx12/BufCopy.cso",
    "reshade-addons/wwmi_dx12/IbVbHash.cso",
    "wwmi_dx12.ini",
}
ADDON_DIR = "reshade-addons"
ADDON_SUBDIR = "reshade-addons/wwmi_dx12"
DXC_SUBDIR = "reshade-addons/wwmi_dx12/dxc"
RT_DATA = 'reshade-addons/rt-pack-data'
OWNED_FILES.update(('dxgi.dll', 'reshade-addons/wwmi_dx12.addon64', 'reshade-addons/zz_wwmi_rt_pack.addon64', 'reshade-addons/rt-pack-data/native-profiles.txt', 'reshade-addons/rt-pack-data/SkinPositions.cso', 'reshade-addons/rt-pack-data/CopyFinalPositions.cso', 'reshade-addons/rt-pack-data/PatchInstances.cso', 'reshade-addons/rt-pack-data/PatchSbt.cso', 'reshade-addons/rt-pack-data/NativeMaterial.dxil', 'reshade-addons/rt-pack-data/SkinTangents.cso', 'reshade-addons/wwmi_dx12/Bc7Half.cso', 'reshade-addons/wwmi_dx12/BufCopy.cso', 'reshade-addons/wwmi_dx12/IbVbHash.cso'))

# Still require the exact relative name AND the real deployment receipt.
OWNED_FILES.update(f"{RT_DATA}/{name}" for name in RT_PACK_SHADERS)
# Backward compatibility for recovery of an existing pre-Batch-1 ledger only.
_PREVIOUS_RT_ASSETS = (
    'raygen-86aa662fab993e04.dxil',
    'raygen-7cd593dfd13f1bd1.dxil',
    'raygen-19c05a81b832ce34.dxil',
    'raygen-f1c3b297855d3694.dxil',
    'raygen-b015ed8b6274ac05.dxil',
    'raygen-6e0e9af8ba6c971b.dxil',
    'raygen-175b6a45890be187.dxil',
    'raygen-7034c5cdd6ae514f.dxil',
    'raygen-b3b7c1d108b3e776.dxil',
    'raygen-fb9ca5a115cc7dc6.dxil',
    'raygen-68a11c5ea87278dd.dxil',
    'raygen-eb8d764f0917ea59.dxil',
    'raygen-8c622780c1eadf7b.dxil',
    'raygen-24f6c4c1cd211904.dxil',
    'raygen-8c7774f2f8b39ab3.dxil',
    'raygen-91987cd6ca87cf6b.dxil',
    'occlusion-default-miss.dxil',
    'NativeSceneMaterialCHS.dxil',
)
OWNED_FILES.update(f"{RT_DATA}/{name}" for name in _PREVIOUS_RT_ASSETS)
OWNED_FILES.update(f"{DXC_SUBDIR}/{name}" for name in ("dxcompiler.dll", "dxil.dll"))

# 只允许补收这类"运行期新增、但旧版账本没记进 runtime_watch"的产物。
# 归属必须由账本快照证明：部署前不存在，才可能是本轮产生的。
ADOPTABLE_RESIDUE = {
    "wwmi_dx12_addon.prev.log": (
        "addon64 内相邻字面量 '.prev.log' 与 'wwmi_dx12_addon.log'："
        "每次加载把上一份日志改名成 .prev.log"
    ),
}
# /proc/comm 会截断到 15 字节，一律比对 cmdline argv[0] 的 basename。
WATCH_EXECUTABLES = (
    "Client-Win64-Shipping.exe",
    "Wuthering Waves.exe",
    "XXMI Launcher.exe",
)


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
    """按 cmdline argv[0] 的 basename 找游戏/XXMI；命名空间隔离时如实报告不可见。"""
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
            continue
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


def trace_inventory(results: Path) -> list[dict]:
    """只读取，不复制、不移动：trace 是证据，原位保留。"""
    if not results.is_dir():
        return []
    items = []
    for path in sorted(results.iterdir()):
        if path.name.startswith("wwmi-observe-") and path.suffix == ".ndjson":
            items.append({"name": path.name, "size": path.stat().st_size})
    return items


def adoptable_residue(record: dict, win64: Path) -> list[dict]:
    """补收旧账本漏记的运行产物；归属必须由部署前快照或时间证明。

    两个可接受证据，任一成立才算本轮产物：
      * 账本 `runtime_snapshot` 里有该键且 `exists=false`；
      * 账本完全没有该键（旧版清单缺口），但文件的北京时间晚于 `started_utc` 且
        部署记录的 `pre_existing_runtime_artifacts` 不含它。
    不满足就跳过并打印原因，绝不凭"文件名看起来像"移动别人的文件。
    """
    from datetime import datetime as _dt, timezone as _tz

    snapshot = record.get("runtime_snapshot") or {}
    pre_existing = set(record.get("pre_existing_runtime_artifacts") or [])
    started = record.get("started_utc")
    started_epoch = None
    if isinstance(started, str):
        try:
            started_epoch = _dt.strptime(started, "%Y-%m-%dT%H:%M:%SZ").replace(
                tzinfo=_tz.utc).timestamp()
        except ValueError:
            started_epoch = None

    adopted = []
    for relative, why in sorted(ADOPTABLE_RESIDUE.items()):
        path = win64 / relative
        if not (path.exists() or path.is_symlink()):
            continue
        if relative in pre_existing:
            print(f"跳过 {relative}：账本记为部署前已存在，不属于本轮")
            continue
        state = snapshot.get(relative)
        if state is not None and state.get("exists"):
            print(f"跳过 {relative}：部署前快照显示已存在，不属于本轮")
            continue
        if state is None:
            if started_epoch is None:
                print(f"跳过 {relative}：旧账本缺该键且无 started_utc，无法证明归属")
                continue
            mtime = path.stat().st_mtime
            if mtime + 1 < started_epoch:
                print(f"跳过 {relative}：修改时间早于本轮部署开始，不属于本轮")
                continue
            proof = f"旧账本缺该键；文件 mtime 晚于 started_utc={started}；{why}"
        else:
            proof = f"部署前快照 exists=false；{why}"
        adopted.append({
            "relative": relative,
            "path": str(path),
            "size": path.stat().st_size if path.is_file() else None,
            "proof": proof,
            "adopted_not_in_watch": True,
        })
    return adopted


def load_manifest(results: Path) -> dict:
    manifest_path = results / "deploy-manifest.json"
    if not manifest_path.is_file():
        raise SystemExit(f"拒绝暂存：找不到本轮账本 {manifest_path}")
    record = json.loads(manifest_path.read_text(encoding="utf-8"))
    if record.get("state") not in {"deployed", "failed"}:
        raise SystemExit(f"拒绝暂存：账本状态不是已部署/部分失败（{record.get('state')}）")
    recovery = record.get("recovery") or {}
    if recovery.get("state") == "recovered":
        raise SystemExit(
            "拒绝暂存：该账本已记录恢复完成 "
            f"（批次 {recovery.get('batch')}，时间 {recovery.get('at')}）"
        )
    return record


def write_manifest(path: Path, record: dict) -> None:
    """回写源账本：只覆盖自身，不删除任何文件。"""
    with path.open("w", encoding="utf-8") as handle:
        json.dump(record, handle, ensure_ascii=False, indent=2)
        handle.write("\n")


def owned_paths(record: dict) -> tuple[list[Path], list[Path]]:
    """从账本推导本轮真正创建的对象；账本外相对路径一律拒绝。

    目录也只认账本 `created_directories` 里记过的；按从深到浅排序，便于整目录搬走。
    """
    win64 = Path(record["game_win64"])
    files: list[Path] = []
    for entry in record.get("files", []):
        relative = entry.get("relative")
        if relative not in OWNED_FILES:
            raise SystemExit(f"拒绝暂存：账本出现非本轮路径 {relative!r}")
        if not entry.get("created"):
            continue
        files.append(win64 / relative)

    dirs: list[Path] = []
    for entry in record.get("created_directories", []):
        relative = entry.get("relative")
        if relative not in (ADDON_DIR, ADDON_SUBDIR, RT_DATA, DXC_SUBDIR):
            raise SystemExit(f"拒绝暂存：账本出现非本轮目录 {relative!r}")
        if not entry.get("created"):
            continue
        dirs.append(win64 / relative)
    return files, dirs


def runtime_paths(record: dict) -> list[Path]:
    """只收回部署前不存在的运行产物与运行目录。"""
    win64 = Path(record["game_win64"])
    pre_existing = set(record.get("pre_existing_runtime_artifacts", []))
    recovered = []
    for relative in record.get("runtime_watch", []):
        if relative in pre_existing:
            continue
        path = win64 / relative
        if path.exists() or path.is_symlink():
            recovered.append(path)
    pre_dirs = set(record.get("pre_existing_runtime_dirs", []))
    dirs = []
    for relative in record.get("runtime_dir_watch", []):
        if relative in pre_dirs:
            continue
        path = win64 / relative
        if path.is_dir():
            dirs.append(path)
    return recovered, dirs


def unexpected_in_dir(record: dict, directory: Path, prefix: str) -> list[Path]:
    """目录内出现账本没记过的条目就整体拒绝，避免搬走别人的文件。"""
    if not directory.is_dir():
        return []
    created = {
        Path(entry["relative"]).name
        for entry in record.get("files", [])
        if entry.get("created") and Path(entry.get("relative", "")).parent.as_posix() == prefix
    }
    # 本轮创建的子目录只认账本 created_directories；不能从 OWNED_FILES 推断，
    # 否则 observer 档会把属于候选档的 wwmi_dx12 当成"本轮目录"而整棵搬走别人的 cso。
    keep_dirs = set()
    for entry in record.get("created_directories", []):
        rel = entry.get("relative", "")
        if entry.get("created") and rel.startswith(prefix + "/"):
            rest = rel[len(prefix) + 1:]
            if rest and "/" not in rest:
                keep_dirs.add(rest)
    unexpected = []
    for child in directory.iterdir():
        if child.is_dir() and child.name in keep_dirs:
            continue  # 仍是本轮的子目录，由各自的清单处理
        if child.name not in created:
            unexpected.append(child)
    return unexpected


def stage(results: Path, batch_name: str | None, dry_run: bool,
          staging_root: Path | None = None) -> int:
    global STAGING_ROOT
    if staging_root is not None:
        STAGING_ROOT = staging_root
    record = load_manifest(results)
    win64 = Path(record["game_win64"])
    profile = record.get("profile", "observer")

    scan = process_scan()
    if scan["detected"]:
        raise SystemExit(
            "拒绝暂存：检测到游戏/XXMI 进程 " + json.dumps(scan["detected"], ensure_ascii=False)
        )

    files, owned_dirs = owned_paths(record)
    runtime_files, runtime_dirs = runtime_paths(record)
    adopted = adoptable_residue(record, win64)
    adopted_paths = [Path(item["path"]) for item in adopted]
    runtime_files = runtime_files + adopted_paths

    # 先查每个受控目录内有没有账本之外的条目。
    for directory, prefix in ((win64 / DXC_SUBDIR, DXC_SUBDIR), (win64 / RT_DATA, RT_DATA),
                              (win64 / ADDON_SUBDIR, ADDON_SUBDIR),
                              (win64 / ADDON_DIR, ADDON_DIR)):
        if not directory.is_dir():
            continue
        # 父目录里的子目录若属于本轮，交给子目录自己的检查。
        strict = unexpected_in_dir(record, directory, prefix)
        if strict:
            listing = "\n".join(f"  {p}" for p in strict)
            raise SystemExit(
                f"拒绝暂存：{directory} 内有本轮账本之外的文件，请先人工核对：\n{listing}"
            )

    missing = [path for path in files if not path.exists() and not path.is_symlink()]
    present = [path for path in files if path.exists() or path.is_symlink()]
    dirs_present = [path for path in owned_dirs if path.is_dir()]

    if not present and not runtime_files and not runtime_dirs and not dirs_present:
        raise SystemExit(f"拒绝暂存：账本内没有仍存在的本轮对象（{win64}）")

    traces = trace_inventory(results)

    if dry_run:
        print(f"[dry-run] 账本：{results / 'deploy-manifest.json'}"
              f"（state={record['state']}, profile={profile}）")
        print(f"[dry-run] 游戏目录：{win64}")
        for path in present:
            print(f"[dry-run] 将暂存 {path}")
        for path in runtime_files:
            print(f"[dry-run] 将暂存运行产物 {path}")
        for item in adopted:
            print(f"[dry-run] 将补收旧账本漏记的产物 {item['path']}（{item['proof']}）")
        for path in runtime_dirs:
            print(f"[dry-run] 将整目录暂存运行目录 {path}")
        for path in sorted(dirs_present, key=lambda x: len(x.parts), reverse=True):
            print(f"[dry-run] 将整目录暂存本轮目录 {path}")
        for path in missing:
            print(f"[dry-run] 账本记为已创建但当前不存在（跳过）{path}")
        for item in traces:
            print(f"[dry-run] trace 保留原位：{item['name']} ({item['size']} 字节)")
        if scan["namespace_scoped"]:
            print(
                "[dry-run] 注意：当前 /proc 处于独立 PID 命名空间，"
                f"只能看到 {scan['visible_pid_count']} 个进程；"
                "宿主上的游戏/XXMI 不可见，不能据此断言都没运行"
            )
        elif not scan["detection_complete"]:
            print("[dry-run] 注意：有进程 cmdline 不可读，未运行状态不完整")
        return 0

    name = batch_name or datetime.now().strftime("batch-%Y%m%d-%H%M%S-wwmi-phaseA")
    batch = STAGING_ROOT / name
    if batch.exists():
        raise SystemExit(f"拒绝暂存：批次目录已存在 {batch}")
    STAGING_ROOT.mkdir(parents=True, exist_ok=True)
    # 暂存根固定 700：只允许本人读写进入。
    if (STAGING_ROOT.stat().st_mode & 0o777) != 0o700:
        STAGING_ROOT.chmod(0o700)
    batch.mkdir(mode=0o700)

    items = []

    def staged_for(path: Path) -> Path:
        """保留相对 EXE 目录的结构，避免同名冲突（wwmi_dx12 目录 vs wwmi_dx12.ini）。"""
        try:
            relative = path.relative_to(win64)
        except ValueError:
            relative = Path(path.name)
        target = batch / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        return target

    # 顺序很重要：先把本轮创建/运行期的整目录搬走（内容已在上方核对全是本轮的），
    # 再搬落在它们外面的单个文件，避免"先建目录导致整目录移动冲突"。
    moved_dirs: list[Path] = []

    def inside_moved(path: Path) -> bool:
        return any(d == path or d in path.parents for d in moved_dirs)

    # 由浅入深：先整棵搬走父目录，嵌在里面的子目录随之进入暂存，不再单独移动。
    for path in sorted(dirs_present + runtime_dirs, key=lambda x: len(x.parts)):
        if not path.is_dir() or inside_moved(path):
            continue
        staged = staged_for(path)
        if staged.exists():
            raise SystemExit(f"拒绝暂存：暂存目标冲突 {staged}")
        kind = ("created_directory_moved_whole"
                if path in dirs_present else "runtime_directory_moved_whole")
        items.append({"original": str(path), "staged": str(staged),
                      "kind": kind, "size": None})
        shutil.move(str(path), str(staged))
        moved_dirs.append(path)

    for path in present + runtime_files:
        if inside_moved(path):
            continue  # 已随整目录进入暂存
        if not (path.exists() or path.is_symlink()):
            continue
        staged = staged_for(path)
        if staged.exists():
            raise SystemExit(f"拒绝暂存：暂存目标冲突 {staged}")
        items.append(
            {
                "original": str(path),
                "staged": str(staged),
                "size": path.stat().st_size if path.is_file() else None,
            }
        )
        shutil.move(str(path), str(staged))

    manifest = {
        "batch": name,
        "created": datetime.now().strftime("%Y-%m-%dT%H:%M:%S"),
        "reason": "Stage5 原生 DX12 首轮实验结束，回滚本轮注入文件与运行期新增产物",
        "source_deploy_manifest": str(results / "deploy-manifest.json"),
        "deploy_state": record["state"],
        "profile": profile,
        "game_win64": str(win64),
        "wwmi_tree": record.get("wwmi_tree"),
        "trace_kept_in_place": str(results),
        "trace_files": traces,
        "adopted_not_in_watch": adopted,
        "skipped_ledger_missing": [str(p) for p in missing],
        "items": items,
    }
    with (batch / "manifest.json").open("x", encoding="utf-8") as handle:
        json.dump(manifest, handle, ensure_ascii=False, indent=2)
        handle.write("\n")

    print(f"已暂存 {len(items)} 项到 {batch}")
    print(f"trace 原位保留：{results}（{len(traces)} 个 ndjson）")
    print("永久删除需要用户明确确认，之后用 clean_staging_batch.py 执行。")
    if adopted:
        print(f"补收旧账本漏记的产物 {len(adopted)} 项：{[i['relative'] for i in adopted]}")

    # 把恢复事实回写到源账本：只新增 recovery 段，原部署字段一字不改。
    # 这样重复运行 recover 会被拒绝，同时保留"当初部署了什么"的历史。
    previous = record.get("recovery") or {}
    history = list(record.get("recovery_history") or [])
    if previous.get("state") == "recovered":
        history.append(previous)
    record["recovery_history"] = history
    record["recovery"] = {
        "state": "recovered",
        "batch": name,
        "batch_manifest": str(batch / "manifest.json"),
        "at": datetime.now().strftime("%Y-%m-%dT%H:%M:%S"),
        "items": len(items),
        "adopted_not_in_watch": [i["relative"] for i in adopted],
    }
    write_manifest(results / "deploy-manifest.json", record)
    print(f"源账本已记录恢复批次：{results / 'deploy-manifest.json'}")
    if missing:
        print(f"账本记为已创建但已不存在：{[str(p) for p in missing]}")
    if record.get("wwmi_tree"):
        print(f"隔离资源树未动：{record['wwmi_tree']}（不属于注入物，保留作证据）")
    if scan["namespace_scoped"]:
        print(
            f"注意：当前 PID 命名空间隔离，只可见 {scan['visible_pid_count']} 个进程；"
            "宿主上的游戏/XXMI 状态需由执行侧另行确认"
        )
    elif not scan["detection_complete"]:
        print("注意：部分进程 cmdline 不可读，未运行状态不完整")
    return 0


def self_test() -> int:
    """测试物放 mkdtemp 并保留，不自动清理。"""
    root = Path(tempfile.mkdtemp(prefix="wwmi-recover-selftest-"))
    global STAGING_ROOT
    original_root = STAGING_ROOT
    STAGING_ROOT = root / "staging"

    OBS_FILES = ("dxgi.dll", "ReShade.ini",
                 "reshade-addons/WWMI_Observer.addon64",
                 "reshade-addons/WWMI_Observer.ini")
    CAND_FILES = ("dxgi.dll", "ReShade.ini", "wwmi_dx12.ini",
                  "reshade-addons/WWMI_DX12_Capture_Stage5.addon64",
                  "reshade-addons/wwmi_dx12/Bc7Half.cso",
                  "reshade-addons/wwmi_dx12/BufCopy.cso",
                  "reshade-addons/wwmi_dx12/IbVbHash.cso")

    def make_observer_win(name: str) -> Path:
        win = root / name
        (win / "reshade-addons").mkdir(parents=True)
        (win / "dxgi.dll").write_bytes(b"dll")
        (win / "ReShade.ini").write_text("[ADDON]\n")
        (win / "ReShade.log").write_text("new log")
        (win / "reshade-addons/WWMI_Observer.addon64").write_bytes(b"addon")
        (win / "reshade-addons/WWMI_Observer.ini").write_text("[observer]\n")
        return win

    def make_candidate_win(name: str) -> Path:
        win = root / name
        (win / "reshade-addons/wwmi_dx12").mkdir(parents=True)
        (win / "dxgi.dll").write_bytes(b"dll")
        (win / "ReShade.ini").write_text("[ADDON]\n")
        (win / "wwmi_dx12.ini").write_text("[wwmi-dx12]\nwwmi_dir=Z:\\iso\n")
        (win / "ReShade.log").write_text("new log")
        (win / "wwmi_dx12_addon.log").write_text("cand log")
        (win / "reshade-addons/WWMI_DX12_Capture_Stage5.addon64").write_bytes(b"cand")
        for cso in ("Bc7Half", "BufCopy", "IbVbHash"):
            (win / f"reshade-addons/wwmi_dx12/{cso}.cso").write_bytes(cso.encode())
        cache = win / "wwmi_dx12/cache/shaders"
        cache.mkdir(parents=True)
        (cache / "x.rfx").write_bytes(b"cache")
        return win

    def obs_record(win: Path, state="deployed", pre=()) -> dict:
        return {
            "state": state, "profile": "observer", "game_win64": str(win),
            "pre_existing_runtime_artifacts": list(pre),
            "runtime_watch": ["ReShade.log", "ReShade.log1", "ReShadePreset.ini"],
            "runtime_dir_watch": [],
            "pre_existing_runtime_dirs": [],
            "created_directories": [{"relative": "reshade-addons", "created": True}],
            "files": [{"relative": r, "created": True} for r in OBS_FILES],
        }

    def cand_record(win: Path, state="deployed", pre=(), pre_dirs=()) -> dict:
        return {
            "state": state, "profile": "candidate", "game_win64": str(win),
            "wwmi_tree": str(root / "iso-tree"),
            "pre_existing_runtime_artifacts": list(pre),
            "runtime_watch": ["ReShade.log", "wwmi_dx12_addon.log"],
            "runtime_dir_watch": ["wwmi_dx12"],
            "pre_existing_runtime_dirs": list(pre_dirs),
            "created_directories": [
                {"relative": "reshade-addons", "created": True},
                {"relative": "reshade-addons/wwmi_dx12", "created": True},
            ],
            "files": [{"relative": r, "created": True} for r in CAND_FILES],
        }

    def write(results: Path, record: dict) -> None:
        results.mkdir(parents=True, exist_ok=True)
        (results / "deploy-manifest.json").write_text(json.dumps(record), encoding="utf-8")
        (results / "wwmi-observe-20261026-000000-p1-0.ndjson").write_text("{}\n")

    try:
        # 1) observer 档：4 文件 + 日志 + 整个 reshade-addons 目录。
        win = make_observer_win("Win64-obs")
        r = root / "obs-ok"
        write(r, obs_record(win))
        assert stage(r, "batch-obs-ok", False) == 0
        assert not (win / "dxgi.dll").exists()
        assert not (win / "reshade-addons").exists()
        assert (r / "wwmi-observe-20261026-000000-p1-0.ndjson").exists()
        b = STAGING_ROOT / "batch-obs-ok"
        assert (b / "dxgi.dll").read_bytes() == b"dll"
        assert (b / "reshade-addons").is_dir()

        # 2) candidate 档：7 文件 + 2 目录 + 运行目录 wwmi_dx12（含 cache）整体搬走。
        win2 = make_candidate_win("Win64-cand")
        r2 = root / "obs-cand"
        write(r2, cand_record(win2))
        assert stage(r2, "batch-cand-ok", False) == 0
        assert not (win2 / "dxgi.dll").exists()
        assert not (win2 / "wwmi_dx12.ini").exists()
        assert not (win2 / "reshade-addons").exists()
        assert not (win2 / "wwmi_dx12").exists(), "运行目录必须整体搬走"
        assert not (win2 / "wwmi_dx12_addon.log").exists()
        b2 = STAGING_ROOT / "batch-cand-ok"
        assert (b2 / "reshade-addons/WWMI_DX12_Capture_Stage5.addon64").read_bytes() == b"cand"
        assert (b2 / "wwmi_dx12.ini").exists()
        assert (b2 / "wwmi_dx12_addon.log").read_text() == "cand log"
        assert (b2 / "wwmi_dx12").is_dir()
        m2 = json.loads((b2 / "manifest.json").read_text())
        assert m2["profile"] == "candidate"
        assert m2["trace_files"][0]["name"].startswith("wwmi-observe-")

        # 3) 部署前已存在的 wwmi_dx12 运行目录与日志：不得移动。
        win3 = make_candidate_win("Win64-pre")
        r3 = root / "obs-pre"
        write(r3, cand_record(win3, pre=["wwmi_dx12_addon.log"], pre_dirs=["wwmi_dx12"]))
        assert stage(r3, "batch-cand-pre", False) == 0
        assert not (win3 / "dxgi.dll").exists()
        assert (win3 / "wwmi_dx12_addon.log").read_text() == "cand log"
        assert (win3 / "wwmi_dx12").is_dir(), "原有运行目录不得被搬走"

        # 4) 账本外文件：整体拒绝，什么都不动，也不建批次。
        win4 = make_candidate_win("Win64-stray")
        stray = win4 / "reshade-addons/wwmi_dx12/other.cso"
        stray.write_bytes(b"x")
        r4 = root / "obs-stray"
        write(r4, cand_record(win4))
        try:
            stage(r4, "batch-cand-stray", False)
        except SystemExit:
            pass
        else:
            raise AssertionError("账本外文件未被拒绝")
        assert (win4 / "dxgi.dll").exists() and stray.exists()
        assert not (STAGING_ROOT / "batch-cand-stray").exists()

        # 5) 部分失败账本：只收账本里 created=true 的对象。
        win5 = root / "Win64-failed"
        (win5 / "reshade-addons").mkdir(parents=True)
        win5.mkdir(exist_ok=True)
        (win5 / "dxgi.dll").write_bytes(b"half")
        r5 = root / "obs-failed"
        rec5 = cand_record(win5, state="failed")
        for f in rec5["files"]:
            f["created"] = (f["relative"] == "dxgi.dll")
        rec5["created_directories"] = [{"relative": "reshade-addons", "created": True}]
        write(r5, rec5)
        assert stage(r5, "batch-cand-failed", False) == 0
        assert not (win5 / "dxgi.dll").exists()
        assert not (win5 / "reshade-addons").exists()
        assert (STAGING_ROOT / "batch-cand-failed" / "dxgi.dll").read_bytes() == b"half"

        # 6) 缺少账本：拒绝。
        r6 = root / "obs-no-manifest"
        r6.mkdir()
        try:
            stage(r6, "batch-none", False)
        except SystemExit:
            pass
        else:
            raise AssertionError("缺账本未被拒绝")

        # 7) 旧账本漏记 .prev.log：按时间归属补收，并在批次与源账本留痕。
        import os
        win7 = make_candidate_win("Win64-adopt")
        prev = win7 / "wwmi_dx12_addon.prev.log"
        prev.write_bytes(b"")
        started = "2026-10-26T10:27:46Z"
        os.utime(prev, (1793015000, 1793015000))  # 晚于 started_utc
        r7 = root / "obs-adopt"
        rec7 = cand_record(win7)
        rec7["started_utc"] = started
        rec7["runtime_snapshot"] = {"wwmi_dx12_addon.log": {"exists": False, "size": None}}
        write(r7, rec7)
        assert stage(r7, "batch-adopt", False) == 0
        assert not prev.exists(), "漏记的 .prev.log 必须被补收"
        b7 = STAGING_ROOT / "batch-adopt"
        assert (b7 / "wwmi_dx12_addon.prev.log").exists()
        m7 = json.loads((b7 / "manifest.json").read_text())
        assert m7["adopted_not_in_watch"][0]["relative"] == "wwmi_dx12_addon.prev.log"
        assert m7["adopted_not_in_watch"][0]["adopted_not_in_watch"] is True
        assert "started_utc" in m7["adopted_not_in_watch"][0]["proof"]
        ledger7 = json.loads((r7 / "deploy-manifest.json").read_text())
        assert ledger7["recovery"]["state"] == "recovered"
        assert ledger7["recovery"]["batch"] == "batch-adopt"
        assert ledger7["state"] == "deployed", "历史部署事实必须保留"
        assert len(ledger7["files"]) == 7, "部署清单不得被改写"

        # 8) 已记录恢复的账本再次 recover：拒绝。
        try:
            stage(r7, "batch-adopt-again", False)
        except SystemExit as exc:
            assert "已记录恢复完成" in str(exc), exc
        else:
            raise AssertionError("重复恢复未被拒绝")

        # 9) 早于本轮部署的 .prev.log 不属于本轮，不得补收。
        win9 = make_candidate_win("Win64-old-prev")
        old = win9 / "wwmi_dx12_addon.prev.log"
        old.write_bytes(b"old")
        os.utime(old, (1793010400, 1793010400))  # 早于 started_utc
        r9 = root / "obs-old-prev"
        rec9 = cand_record(win9)
        rec9["started_utc"] = started
        write(r9, rec9)
        assert stage(r9, "batch-old-prev", False) == 0
        assert old.read_text() == "old", "部署前就存在的文件不得被搬走"
        m9 = json.loads((STAGING_ROOT / "batch-old-prev/manifest.json").read_text())
        assert m9["adopted_not_in_watch"] == []
    finally:
        STAGING_ROOT = original_root

    print(f"self-test ok（测试物保留在 {root}）")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--results")
    parser.add_argument("--batch")
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("--self-test", action="store_true")
    parser.add_argument("--staging-root", help="仅用于本地替身测试：覆盖删除暂存根目录")
    args = parser.parse_args()
    if args.self_test:
        return self_test()
    if not args.results:
        parser.error("需要 --results（或 --self-test）")
    return stage(Path(args.results), args.batch, args.dry_run,
                 Path(args.staging_root) if args.staging_root else None)


if __name__ == "__main__":
    raise SystemExit(main())
