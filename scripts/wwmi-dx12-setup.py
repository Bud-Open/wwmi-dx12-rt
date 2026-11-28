#!/usr/bin/env python3
"""WWMI-DX12-RT Linux / SteamOS / Cross-Platform CLI & Native Dialog Setup Tool.

Zero third-party Python dependencies (uses Python 3 stdlib + optional kdialog/zenity for GUI).
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO_ROOT = HERE.parent if (HERE.parent / "tools/installer_core.py").is_file() else HERE
sys.path.insert(0, str(REPO_ROOT / "tools"))

import installer_core as core  # noqa: E402


def resolve_defaults(args: argparse.Namespace) -> tuple[Path | None, Path | None, Path, core.GameInstall | None]:
    installs = core.discover_game_installs()
    matched_install: core.GameInstall | None = installs[0] if installs else None

    win64_dir: Path | None = None
    if getattr(args, "game_win64", None):
        p = Path(args.game_win64).expanduser().resolve()
        # Accept either outer game dir or exact Win64 dir
        hit = core._probe_game_root_candidates(p)
        win64_dir = hit[1] if hit else p
        matched_install = None
        for inst in installs:
            if inst.win64_dir == win64_dir:
                matched_install = inst
                break
    elif matched_install:
        win64_dir = matched_install.win64_dir

    if getattr(args, "base_root", None):
        base_root = Path(args.base_root).expanduser().resolve()
    elif sys.platform == "win32":
        base_root = REPO_ROOT.resolve()
    else:
        base_root = core.select_default_linux_base_root(matched_install)

    active_ledger = None
    try:
        loaded = core.load_install_ledger(base_root)
        if loaded and loaded.get("state") in core.ACTIVE_STATES:
            active_ledger = loaded
    except RuntimeError:
        active_ledger = None

    if not getattr(args, "game_win64", None) and active_ledger and active_ledger.get("win64_dir"):
        recorded_win64 = Path(active_ledger["win64_dir"])
        if recorded_win64.is_dir():
            win64_dir = recorded_win64

    wwmi_dirs = core.discover_wwmi_dirs(install=matched_install)
    wwmi_dir: Path | None = None
    p_kind = "windows" if sys.platform == "win32" else "linux-proton"
    if getattr(args, "wwmi_dir", None):
        wwmi_dir = Path(args.wwmi_dir).expanduser().resolve()
    else:
        active_wwmi = core.resolve_active_wwmi_dir_from_base(base_root, active_ledger, p_kind)
        if active_wwmi and active_wwmi.is_dir():
            wwmi_dir = active_wwmi
        else:
            if win64_dir:
                migrate_from = Path(args.migrate_from).expanduser().resolve() if getattr(args, "migrate_from", None) else None
                owner, _ = core.detect_verified_win64_owner(win64_dir, base_root, p_kind, migrate_from)
                if owner is not None:
                    prev_wwmi = core.resolve_owner_wwmi_dir(owner, p_kind)
                    if prev_wwmi and (prev_wwmi / "d3dx.ini").is_file():
                        wwmi_dir = prev_wwmi
            if wwmi_dir is None and wwmi_dirs:
                wwmi_dir = wwmi_dirs[0]

    return win64_dir, wwmi_dir, base_root, matched_install


def print_doctor_report(rep: core.DoctorReport) -> None:
    icons = {"ok": "[OK]  ", "warn": "[WARN]", "error": "[ERR] "}
    print(core.tr("=== WWMI-DX12-RT 环境与健康自检报告 ===", "=== WWMI-DX12-RT Environment & Health Check ==="))
    print(f"  {core.tr('当前包版本     ', 'Package Version')} : {rep.package_version}")
    if rep.installed_version:
        print(f"  {core.tr('已安装版本     ', 'Installed Ver  ')} : {rep.installed_version}")
    print(f"  {core.tr('游戏 Win64 目录', 'Game Win64 Dir ')} : {rep.win64_dir}")
    print(f"  {core.tr('WWMI Mod 目录  ', 'WWMI Mods Dir  ')} : {rep.wwmi_dir}")
    print(f"  {core.tr('外部挂载根目录 ', 'External Root  ')} : {rep.base_root}")
    print(f"  {core.tr('当前挂载状态   ', 'Mount Status   ')} : {rep.mount_state}")
    print("----------------------------------------")
    for f in rep.findings:
        print(f"  {icons.get(f.level, '[INFO]')} {f.message}")
    print("========================================")


def cmd_doctor(args: argparse.Namespace) -> int:
    win64_dir, wwmi_dir, base_root, inst = resolve_defaults(args)
    if not win64_dir:
        print(core.tr(
            "未自动检测到《鸣潮》安装目录，请通过 --game-win64 指定路径。",
            "Could not auto-detect Wuthering Waves; pass --game-win64 explicitly.",
        ), file=sys.stderr)
        return 2
    if not wwmi_dir:
        wwmi_dir = base_root / "WWMI"
    migrate_from = Path(args.migrate_from).expanduser().resolve() if getattr(args, "migrate_from", None) else None
    rep = core.run_doctor(
        win64_dir=win64_dir,
        wwmi_dir=wwmi_dir,
        base_root=base_root,
        compatdata_dir=inst.compatdata_dir if inst else None,
        is_flatpak_steam=inst.is_flatpak_steam if inst else False,
        source_root=REPO_ROOT,
        migrate_from=migrate_from,
    )
    if args.json:
        print(json.dumps({"healthy": rep.healthy, **rep.__dict__, "findings": [f.__dict__ for f in rep.findings]}, indent=2, ensure_ascii=False))
    else:
        print_doctor_report(rep)
    return 0 if rep.healthy else 1


def cmd_init_wwmi(args: argparse.Namespace) -> int:
    target = Path(args.target).expanduser().resolve()
    out = core.init_wwmi_skeleton(target)
    print(core.tr(f"已初始化标准 WWMI 目录骨架：{out}", f"Initialized standard WWMI skeleton: {out}"))
    print(core.tr(
        f"  - 请将官方 WWMI-Package 的 Core/ 与 RabbitFX 放入该目录，角色皮肤解压至 {out / 'Mods'}",
        f"  - Copy official WWMI-Package Core/ and RabbitFX here, and extract skins under {out / 'Mods'}",
    ))
    return 0


def cmd_install(args: argparse.Namespace) -> int:
    win64_dir, wwmi_dir, base_root, inst = resolve_defaults(args)
    if not win64_dir:
        print(core.tr(
            "错误：未找到《鸣潮》Win64 目录，请使用 --game-win64 指定。",
            "Error: Wuthering Waves Win64 directory not found; specify --game-win64.",
        ), file=sys.stderr)
        return 2
    dry_run = getattr(args, "dry_run", False)
    migrate_from = Path(args.migrate_from).expanduser().resolve() if getattr(args, "migrate_from", None) else None
    init_wwmi = not wwmi_dir and getattr(args, "init_missing_wwmi", False)
    if not wwmi_dir:
        if not init_wwmi:
            print(core.tr(
                "错误：未找到现有 WWMI 目录，请使用 --wwmi-dir 指定或添加 --init-missing-wwmi。",
                "Error: No existing WWMI directory found; pass --wwmi-dir or add --init-missing-wwmi.",
            ), file=sys.stderr)
            return 2
        wwmi_dir = base_root / "WWMI"

    res = core.install_mount(
        source_root=REPO_ROOT,
        win64_dir=win64_dir,
        wwmi_dir=wwmi_dir,
        base_root=base_root,
        skin_mode=getattr(args, "skin_mode", "base"),
        proton_utf8_guard=getattr(args, "proton_utf8_guard", False),
        compatdata_dir=inst.compatdata_dir if inst else None,
        dry_run=dry_run,
        init_missing_wwmi=init_wwmi,
        migrate_from=migrate_from,
        is_flatpak_steam=inst.is_flatpak_steam if inst else False,
        skin_mode_explicit=getattr(args, "_skin_mode_explicit", False),
    )
    if dry_run:
        print(core.tr("[dry-run] 安装计划校验通过（未写入任何文件）：", "[dry-run] Install plan verified (no files written):"))
        print(json.dumps(res, indent=2, ensure_ascii=False))
    else:
        mode = res.get("install_mode", "fresh")
        mode_labels = {
            "upgrade": core.tr("并排升级完成！（旧目录已完整保留，可随时回滚）", "Side-by-side upgrade complete! (Previous folder preserved for rollback)"),
            "rollback": core.tr("回滚完成！（原目录已完整保留）", "Rollback complete! (Previous folder preserved)"),
            "switch": core.tr("并排切换完成！（原目录已完整保留）", "Side-by-side switch complete! (Previous folder preserved)"),
            "refresh": core.tr("原地刷新完成！（已保留现有外部配置）", "In-place refresh complete! (External configuration preserved)"),
            "repair": core.tr("修复挂载完成！", "Mount repaired and enabled!"),
            "fresh": core.tr("安装启用完成！", "Installed and enabled!"),
        }
        print(mode_labels.get(mode, core.tr("安装启用完成！", "Installed and enabled!")))
        print(f"  - {core.tr('已安装版本', 'Installed ver')}: {res.get('installed_version', 'dev')}")
        print(f"  - {core.tr('外部程序根', 'External root')}: {res['base_root']}")
        if res.get("upgraded_from"):
            print(f"  - {core.tr('迁移来源目录', 'Migrated from')}: {res['upgraded_from']}")
        print(f"  - {core.tr('关联 Mod 树', 'WWMI tree')}: {res['wwmi_dir']}")
        print(f"  - {core.tr('游戏引导桥', 'Game Win64')}: {res['win64_dir']} ({core.tr('仅 dxgi.dll + ReShade.ini', 'dxgi.dll + ReShade.ini only')})")
        if res.get("migrated_cache"):
            print(f"  - {core.tr('已迁移缓存', 'Migrated cache')}: {', '.join(res['migrated_cache'])}")
        if res.get("migrated_addons"):
            print(f"  - {core.tr('已迁移第三方插件', 'Migrated add-ons')}: {', '.join(res['migrated_addons'])}")
        for warn in res.get("migration_warnings") or []:
            print(f"  [WARN] {warn}")
        if not (Path(res["wwmi_dir"]) / "Core/WWMI/WuWa-Model-Importer.ini").is_file():
            print(core.tr(
                "  [提示] 当前 WWMI 目录尚未放入官方 Core/WWMI 核心包，请补齐后再把皮肤解压到 Mods/（若皮肤需要 RabbitFX 请仅保留一份）。",
                "  [Note] Core/WWMI scripts not detected yet. Extract official WWMI-Package Core/ here before using skins (keep at most one RabbitFX under Mods/).",
            ))
    if res.get("proton_launch_options"):
        print(f"{core.tr('Steam → 游戏属性 → 启动选项', 'Steam -> Properties -> Launch Options')}: {res['proton_launch_options']}")
    else:
        print(core.tr("可以从 Steam 或库洛官方启动器开始游戏。", "You can now launch the game normally from Steam or the Kuro Launcher."))
    if res.get("proton_preload_warning"):
        print(f"[WARN] {res['proton_preload_warning']}")
    return 0


def _run_uninstall(args: argparse.Namespace) -> tuple[int, str]:
    if getattr(args, "base_root", None):
        base_root = Path(args.base_root).expanduser().resolve()
    else:
        _, _, base_root, _ = resolve_defaults(args)
    res = core.uninstall_mount(base_root=base_root, dry_run=args.dry_run)
    if args.dry_run:
        print(core.tr("[dry-run] 卸载预览：", "[dry-run] Uninstall preview:"))
        print(json.dumps(res, indent=2, ensure_ascii=False))
        return 0, core.tr("卸载预检完成，未修改文件。", "Uninstall dry-run complete; no files modified.")
    if res.get("state") == "noop":
        msg = res["message"]
        print(msg)
        return 0, msg
    lines = [core.tr(
        f"已安全卸载还原游戏目录：{res.get('win64_dir')}",
        f"Safely uninstalled and restored game directory: {res.get('win64_dir')}",
    )]
    if res.get("staged_batch"):
        lines.append(core.tr(
            f"移除的引导文件已安全暂存至：{res['staged_batch']}",
            f"Removed bootstrap files staged at: {res['staged_batch']}",
        ))
    if res.get("has_legacy_settings"):
        lines.append(core.tr(
            "旧版账本中的 compatdata 设置已保留；请按需人工检查旧配置和 Steam 启动选项。",
            "Legacy compatdata settings recorded in the old ledger were preserved; review old settings and Steam Launch Options if needed.",
        ))
    msg = "\n".join(lines)
    print(msg)
    return 0, msg


def _run_rollback(args: argparse.Namespace) -> tuple[int, str]:
    if getattr(args, "base_root", None):
        base_root = Path(args.base_root).expanduser().resolve()
    else:
        _, _, base_root, _ = resolve_defaults(args)
    target = Path(args.to).expanduser().resolve() if getattr(args, "to", None) else None
    res = core.rollback_mount(base_root=base_root, target_root=target, dry_run=getattr(args, "dry_run", False))
    if getattr(args, "dry_run", False):
        print(core.tr("[dry-run] 回滚预览：", "[dry-run] Rollback preview:"))
        print(json.dumps(res, indent=2, ensure_ascii=False))
        return 0, core.tr("回滚预检完成，未修改文件。", "Rollback dry-run complete; no files modified.")
    lines = [core.tr(
        f"已安全回滚至旧目录：{res['to_root']}（版本：{res['target_version']}）",
        f"Safely rolled back to previous folder: {res['to_root']} (version: {res['target_version']})",
    )]
    if res.get("proton_launch_options"):
        lines.append(core.tr(
            f"若 Steam 启动选项使用了 LD_PRELOAD，请确认指向旧目录：\n{res['proton_launch_options']}",
            f"If Steam Launch Options use LD_PRELOAD, verify it points back to the old folder:\n{res['proton_launch_options']}",
        ))
    msg = "\n".join(lines)
    print(msg)
    return 0, msg


def cmd_rollback(args: argparse.Namespace) -> int:
    rc, _ = _run_rollback(args)
    return rc


def cmd_uninstall(args: argparse.Namespace) -> int:
    rc, _ = _run_uninstall(args)
    return rc


def format_update_check_text(info: dict) -> str:
    lines = [
        core.tr("=== WWMI-DX12-RT 更新检查结果 ===", "=== WWMI-DX12-RT Update Check ==="),
        f"  {core.tr('当前版本', 'Current Version')} : {info['current_version']}",
        f"  {core.tr('最新版本', 'Latest Release ')} : {info['latest_version'] or core.tr('未知', 'Unknown')}",
    ]
    if info.get("published_at"):
        lines.append(f"  {core.tr('发布时间', 'Published At   ')} : {info['published_at']}")
    if info.get("update_available"):
        lines.append(core.tr(
            "  状态：发现新版本！可前往 GitHub Releases 下载并解压至新目录执行并排升级。",
            "  Status: A newer release is available! Download from GitHub Releases and extract side-by-side to upgrade.",
        ))
    elif core.parse_semver(info.get("current_version")) is None:
        lines.append(core.tr(
            "  状态：当前为开发/自定义版本，请对照上方最新正式版自行判断是否更新。",
            "  Status: Current build is a dev/custom version; compare against the latest release above.",
        ))
    else:
        lines.append(core.tr(
            "  状态：当前已是最新版本。",
            "  Status: You are already on the latest release.",
        ))
    if info.get("asset_name"):
        lines.append(f"  {core.tr('推荐压缩包', 'Package Asset  ')} : {info['asset_name']}")
    if info.get("asset_digest"):
        lines.append(f"  {core.tr('资产校验值', 'Asset Digest   ')} : {info['asset_digest']}")
    lines.append(f"  {core.tr('发布页面', 'Release Page   ')} : {info['release_url']}")
    return "\n".join(lines)


def cmd_check_update(args: argparse.Namespace) -> int:
    current_version = core.read_package_provenance(REPO_ROOT)["version"] or "dev"
    p_kind = "windows" if sys.platform == "win32" else "linux-proton"
    info = core.check_latest_release(current_version, platform_kind=p_kind)
    if getattr(args, "json", False):
        print(json.dumps(info, indent=2, ensure_ascii=False))
    else:
        print(format_update_check_text(info))
    return 0


def _open_release_url(url: str) -> None:
    if not core.is_allowed_github_release_url(url):
        return
    opener = shutil.which("xdg-open")
    if opener:
        subprocess.Popen([opener, url], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def cmd_desktop_entry(args: argparse.Namespace) -> int:
    apps_dir = Path.home() / ".local/share/applications"
    apps_dir.mkdir(parents=True, exist_ok=True)
    desktop_file = apps_dir / "wwmi-dx12-rt-setup.desktop"
    script_path = (REPO_ROOT / "wwmi-dx12-setup").resolve()
    if not script_path.exists():
        script_path = Path(__file__).resolve()
    content = (
        "[Desktop Entry]\n"
        "Type=Application\n"
        f"Name={core.tr('WWMI DX12+RT 配置器', 'WWMI DX12+RT Setup')}\n"
        f"Comment={core.tr('鸣潮 DX12 + 光追 Mod 外部挂载安装与卸载工具', 'Wuthering Waves DX12 + RT external mount setup & uninstaller')}\n"
        f'Exec="{sys.executable}" "{Path(__file__).resolve()}" gui\n'
        "Icon=applications-games\n"
        "Terminal=false\n"
        "Categories=Game;Utility;\n"
    )
    desktop_file.write_text(content, encoding="utf-8")
    desktop_file.chmod(0o755)
    print(core.tr(f"已生成桌面应用入口：{desktop_file}", f"Created desktop entry: {desktop_file}"))
    return 0


def gui_install(args: argparse.Namespace, base_root: Path) -> tuple[int, str]:
    args.init_missing_wwmi = True
    rc = cmd_install(args)
    if rc != 0:
        return rc, core.tr(
            "安装未完成：未找到游戏或 WWMI 目录，请查看终端输出。",
            "Installation incomplete: game or WWMI directory not found (check terminal output).",
        )
    if args.dry_run:
        return rc, core.tr("安装预检通过，未写入文件。", "Install dry-run succeeded; no files written.")
    message = core.tr("安装启用成功。", "Installed and enabled successfully.")
    _, wwmi_dir, resolved_base, _ = resolve_defaults(args)
    target_wwmi = wwmi_dir or (resolved_base / "WWMI")
    if not (target_wwmi / "Core/WWMI/WuWa-Model-Importer.ini").is_file():
        message += core.tr(
            f"\n\n[提示] 尚未检测到官方 Core/WWMI 核心包，请将其放入：\n{target_wwmi}",
            f"\n\n[Note] Official Core/WWMI scripts not detected yet; place them inside:\n{target_wwmi}",
        )
    if sys.platform != "win32":
        options, warning = core.proton_launch_options(base_root, args.proton_utf8_guard)
        message += core.tr(
            f"\n\nSteam → 游戏属性 → 启动选项（请确保使用 DX12 启动）：\n{options}",
            f"\n\nSteam -> Properties -> Launch Options (ensure DX12 mode):\n{options}",
        )
        if warning:
            message += f"\n\n{warning}"
    return rc, message


def cmd_gui(args: argparse.Namespace) -> int:
    """Interactive wizard via kdialog / zenity (for Linux desktop & Steam Deck) with TUI fallback."""
    win64_dir, wwmi_dir, base_root, inst = resolve_defaults(args)
    migrate_from = Path(args.migrate_from).expanduser().resolve() if getattr(args, "migrate_from", None) else None
    action_info = (
        core.describe_install_action(win64_dir, base_root, source_root=REPO_ROOT, migrate_from=migrate_from)
        if win64_dir
        else {"action": "install", "mount_state": "not_installed", "message": core.tr("未检测到游戏目录", "Game directory not detected")}
    )
    state = action_info["mount_state"]
    state_msg = action_info["message"]
    pkg_ver = core.read_package_provenance(REPO_ROOT)["version"] or "dev"
    installed_ver_line = ""
    can_rollback_prev: str | None = None
    try:
        loaded_ledger = core.load_install_ledger(base_root)
        if loaded_ledger and loaded_ledger.get("state") in core.ACTIVE_STATES:
            installed_ver_line = f"{core.tr('已安装版本', 'Installed Ver')}: {core.format_installed_version(loaded_ledger)}\n"
            if loaded_ledger.get("state") == "installed" and loaded_ledger.get("upgraded_from"):
                cand_prev = Path(loaded_ledger["upgraded_from"])
                if cand_prev.is_dir() and (cand_prev / "host/ReShade64-game-forward.dll").is_file():
                    can_rollback_prev = str(cand_prev)
            elif loaded_ledger.get("state") == "rolling_back" and loaded_ledger.get("rollback_target"):
                cand_prev = Path(loaded_ledger["rollback_target"])
                if cand_prev.is_dir() and (cand_prev / "host/ReShade64-game-forward.dll").is_file():
                    can_rollback_prev = str(cand_prev)
    except RuntimeError:
        pass
    summary = (
        f"{core.tr('当前包版本', 'Package Version')}: {pkg_ver}\n"
        f"{installed_ver_line}"
        f"{core.tr('当前状态', 'Status')}: {state} ({state_msg})\n"
        f"{core.tr('游戏目录', 'Game Dir')}: {win64_dir or core.tr('未检测到', 'Not detected')}\n"
        f"{core.tr('WWMI 目录', 'WWMI Dir')}: {wwmi_dir or core.tr('未检测到 (安装时将初始化)', 'Not detected (will initialize on install)')}\n"
        f"{core.tr('外部挂载根', 'External Root')}: {base_root}"
    )

    title = core.tr("WWMI DX12+RT 安装配置器", "WWMI DX12+RT Setup")
    act_kind = action_info.get("action", "install")
    src_ver = action_info.get("source_version") or ""
    if act_kind == "upgrade":
        opt_install = core.tr(
            f"并排升级 ({src_ver} -> {pkg_ver}，保留旧目录可随时回滚)",
            f"Side-by-Side Upgrade ({src_ver} -> {pkg_ver}, preserves old folder for rollback)",
        )
    elif act_kind == "rollback":
        opt_install = core.tr(
            f"回滚至此版本 ({src_ver} -> {pkg_ver})",
            f"Rollback to This Version ({src_ver} -> {pkg_ver})",
        )
    elif act_kind == "switch":
        opt_install = core.tr(
            "并排切换至当前目录（保留原目录）",
            "Side-by-Side Switch to This Folder (preserves old folder)",
        )
    elif act_kind == "refresh":
        opt_install = core.tr(
            f"原地刷新 ({pkg_ver}，不保留旧版载荷回滚)",
            f"In-Place Refresh ({pkg_ver}, overwrites payload in same folder)",
        )
    else:
        opt_install = core.tr(
            "一键安装启用（原习惯从 Steam / 官方启动器直启）",
            "Install & Enable (launch normally via Steam / Kuro Launcher)",
        )
    opt_uninstall = core.tr("一键卸载还原（恢复 100% 纯净游戏目录）", "Uninstall / Restore (restore clean Win64 directory)")
    opt_rollback = (
        core.tr(f"一键回滚至旧版目录（{can_rollback_prev}）", f"Rollback to Previous Folder ({can_rollback_prev})")
        if can_rollback_prev
        else ""
    )
    opt_doctor = core.tr("运行环境与 RabbitFX 健康自检", "Run environment & RabbitFX health check")
    opt_update = core.tr("检查更新（打开 GitHub 发布页）", "Check for Updates (Open GitHub Releases)")

    kdialog = shutil.which("kdialog")
    zenity = shutil.which("zenity")

    if kdialog:
        menu_items = ["install", opt_install]
        if can_rollback_prev:
            menu_items.extend(["rollback", opt_rollback])
        menu_items.extend(["uninstall", opt_uninstall, "doctor", opt_doctor, "check-update", opt_update])
        choice = subprocess.run(
            [
                kdialog,
                "--title",
                title,
                "--menu",
                f"{summary}\n\n{core.tr('请选择要执行的操作：', 'Choose an action:')}",
                *menu_items,
            ],
            capture_output=True,
            text=True,
        )
        if choice.returncode != 0:
            return 0
        act = choice.stdout.strip()
        try:
            if act == "install":
                rc, message = gui_install(args, base_root)
                subprocess.run([kdialog, "--error" if rc else "--msgbox", message])
                return rc
            if act == "rollback":
                args.dry_run = False
                args.to = None
                rc, msg = _run_rollback(args)
                subprocess.run([kdialog, "--msgbox", msg])
                return rc
            if act == "uninstall":
                args.dry_run = False
                rc, msg = _run_uninstall(args)
                subprocess.run([kdialog, "--msgbox", msg])
                return rc
            if act == "doctor" and win64_dir:
                rep = core.run_doctor(win64_dir, wwmi_dir or (base_root / "WWMI"), base_root,
                                      compatdata_dir=inst.compatdata_dir if inst else None,
                                      is_flatpak_steam=inst.is_flatpak_steam if inst else False,
                                      source_root=REPO_ROOT, migrate_from=migrate_from)
                msg = "\n".join(f"[{f.level.upper()}] {f.message}" for f in rep.findings)
                subprocess.run([kdialog, "--msgbox", msg])
                return 0
            if act == "check-update":
                info = core.check_latest_release(pkg_ver, "windows" if sys.platform == "win32" else "linux-proton")
                msg = format_update_check_text(info) + core.tr(
                    "\n\n是否在浏览器中打开 GitHub Releases 发布页？",
                    "\n\nOpen the GitHub Releases page in your browser?",
                )
                ans = subprocess.run([kdialog, "--yesno", msg])
                if ans.returncode == 0:
                    _open_release_url(info["release_url"])
                return 0
        except Exception as exc:
            subprocess.run([kdialog, "--error", str(exc)])
            return 1

    elif zenity:
        z_items = ["install", opt_install]
        if can_rollback_prev:
            z_items.extend(["rollback", opt_rollback])
        z_items.extend(["uninstall", opt_uninstall, "doctor", opt_doctor, "check-update", opt_update])
        choice = subprocess.run(
            [
                zenity,
                "--list",
                f"--title={title}",
                f"--text={summary}",
                f"--column={core.tr('代码', 'Action')}",
                f"--column={core.tr('操作说明', 'Description')}",
                *z_items,
                "--width=680",
                "--height=400",
            ],
            capture_output=True,
            text=True,
        )
        if choice.returncode != 0:
            return 0
        act = choice.stdout.strip()
        try:
            if act == "install":
                rc, message = gui_install(args, base_root)
                subprocess.run([zenity, "--error" if rc else "--info", f"--text={message}"])
                return rc
            if act == "rollback":
                args.dry_run = False
                args.to = None
                rc, msg = _run_rollback(args)
                subprocess.run([zenity, "--info", f"--text={msg}"])
                return rc
            if act == "uninstall":
                args.dry_run = False
                rc, msg = _run_uninstall(args)
                subprocess.run([zenity, "--info", f"--text={msg}"])
                return rc
            if act == "doctor" and win64_dir:
                rep = core.run_doctor(win64_dir, wwmi_dir or (base_root / "WWMI"), base_root,
                                      compatdata_dir=inst.compatdata_dir if inst else None,
                                      is_flatpak_steam=inst.is_flatpak_steam if inst else False,
                                      source_root=REPO_ROOT, migrate_from=migrate_from)
                msg = "\n".join(f"[{f.level.upper()}] {f.message}" for f in rep.findings)
                subprocess.run([zenity, "--info", f"--text={msg}"])
                return 0
            if act == "check-update":
                info = core.check_latest_release(pkg_ver, "windows" if sys.platform == "win32" else "linux-proton")
                msg = format_update_check_text(info) + core.tr(
                    "\n\n是否在浏览器中打开 GitHub Releases 发布页？",
                    "\n\nOpen the GitHub Releases page in your browser?",
                )
                ans = subprocess.run([zenity, "--question", f"--text={msg}"])
                if ans.returncode == 0:
                    _open_release_url(info["release_url"])
                return 0
        except Exception as exc:
            subprocess.run([zenity, "--error", f"--text={exc}"])
            return 1

    # Terminal interactive fallback
    print(summary)
    if can_rollback_prev:
        print(core.tr(
            "\n1) 一键安装/升级   2) 一键卸载还原   3) 健康自检   4) 检查更新   5) 回滚旧版   q) 退出",
            "\n1) Install / Upgrade   2) Uninstall / Restore   3) Health Check   4) Check Updates   5) Rollback   q) Quit",
        ))
        sel = input(core.tr("请输入选项 [1/2/3/4/5/q]: ", "Enter choice [1/2/3/4/5/q]: ")).strip().lower()
    else:
        print(core.tr(
            "\n1) 一键安装/升级   2) 一键卸载还原   3) 健康自检   4) 检查更新   q) 退出",
            "\n1) Install / Upgrade   2) Uninstall / Restore   3) Health Check   4) Check Updates   q) Quit",
        ))
        sel = input(core.tr("请输入选项 [1/2/3/4/q]: ", "Enter choice [1/2/3/4/q]: ")).strip().lower()
    if sel == "1":
        rc, _ = gui_install(args, base_root)
        return rc
    if sel == "5" and can_rollback_prev:
        args.dry_run = False
        args.to = None
        return cmd_rollback(args)
    if sel == "2":
        args.dry_run = False
        return cmd_uninstall(args)
    if sel == "3":
        args.json = False
        return cmd_doctor(args)
    if sel == "4":
        args.json = False
        return cmd_check_update(args)
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="WWMI-DX12-RT cross-platform external mount installer & doctor")
    p.add_argument("--game-win64", help="Wuthering Waves root or Client/Binaries/Win64 path (auto-detected by default)")
    p.add_argument("--wwmi-dir", help="WWMI directory containing d3dx.ini and Mods/ (auto-detected by default)")
    p.add_argument("--base-root", help="External runtime/cache root (default ~/.local/share/wwmi-dx12-rt on Linux)")
    p.add_argument("--migrate-from", help="Explicit previous external base_root to verify & migrate from when Win64 stub cannot be reverse-mapped")
    p.add_argument("--lang", choices=("zh", "en"), help="Override UI/output language (zh or en)")

    shared = argparse.ArgumentParser(add_help=False)
    shared.add_argument("--game-win64", default=argparse.SUPPRESS)
    shared.add_argument("--wwmi-dir", default=argparse.SUPPRESS)
    shared.add_argument("--base-root", default=argparse.SUPPRESS)
    shared.add_argument("--migrate-from", default=argparse.SUPPRESS)
    shared.add_argument("--lang", choices=("zh", "en"), default=argparse.SUPPRESS)

    sub = p.add_subparsers(dest="cmd")

    p_doc = sub.add_parser("doctor", aliases=["status"], parents=[shared],
                           help="Read-only health check for game path, mount state, WWMI & RabbitFX")
    p_doc.add_argument("--json", action="store_true", help="Output report as JSON")

    p_ins = sub.add_parser("install", parents=[shared],
                           help="Deploy external runtime and mount 2 bootstrap files into Win64")
    def add_install_options(parser: argparse.ArgumentParser, skin_default: str) -> None:
        parser.add_argument("--skin-mode", choices=("default", "base", "exact"), default=skin_default,
                            help=f"Hybrid raster skinning mode (default: {skin_default})")
        parser.add_argument("--init-missing-wwmi", action="store_true", help="Initialize WWMI skeleton if missing")
        parser.add_argument("--proton-utf8-guard", action="store_true", help="Print Steam Launch Options with UTF-8 guard")
        parser.add_argument("--dry-run", action="store_true", help="Validate plan without writing files")

    add_install_options(p_ins, "base")

    p_un = sub.add_parser("uninstall", parents=[shared],
                          help="Safely stage Win64 bootstrap files back out using the install ledger")
    p_un.add_argument("--dry-run", action="store_true", help="Preview files to stage without moving them")

    p_rb = sub.add_parser("rollback", parents=[shared],
                          help="Roll back an upgraded install to its preserved previous folder (supports v1.0.2/v1.0.3)")
    p_rb.add_argument("--to", help="Explicit previous folder to restore (defaults to ledger upgraded_from)")
    p_rb.add_argument("--dry-run", action="store_true", help="Validate rollback plan without writing files")

    p_init = sub.add_parser("init-wwmi", parents=[shared], help="Initialize a standard WWMI folder skeleton")
    p_init.add_argument("target", help="Target directory path")

    p_gui = sub.add_parser("gui", parents=[shared], help="Launch interactive GUI wizard (kdialog/zenity) or TUI")
    add_install_options(p_gui, "default")
    p_upd = sub.add_parser("check-update", parents=[shared],
                           help="Opt-in check against GitHub Releases for a newer WWMI-DX12-RT version")
    p_upd.add_argument("--json", action="store_true", help="Output update info as JSON")
    sub.add_parser("desktop-entry", parents=[shared], help="Create a desktop menu entry in ~/.local/share/applications")
    return p


def main(argv: list[str] | None = None) -> int:
    raw_argv = list(sys.argv[1:] if argv is None else argv)
    parser = build_parser()
    args = parser.parse_args(raw_argv)
    args._skin_mode_explicit = any(a == "--skin-mode" or a.startswith("--skin-mode=") for a in raw_argv)
    if getattr(args, "lang", None):
        os.environ["WWMI_SETUP_LANG"] = args.lang
    cmd = args.cmd or "doctor"
    if cmd in ("doctor", "status"):
        if not hasattr(args, "json"):
            args.json = False
        return cmd_doctor(args)
    if cmd == "install":
        return cmd_install(args)
    if cmd == "uninstall":
        return cmd_uninstall(args)
    if cmd == "rollback":
        return cmd_rollback(args)
    if cmd == "init-wwmi":
        return cmd_init_wwmi(args)
    if cmd == "gui":
        return cmd_gui(args)
    if cmd == "check-update":
        return cmd_check_update(args)
    if cmd == "desktop-entry":
        return cmd_desktop_entry(args)
    parser.print_help()
    return 64


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"{core.tr('错误：', 'Error: ')}{exc}", file=sys.stderr)
        sys.exit(1)
