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
    if getattr(args, "wwmi_dir", None):
        wwmi_dir = Path(args.wwmi_dir).expanduser().resolve()
    elif active_ledger and active_ledger.get("wwmi_dir") and Path(active_ledger["wwmi_dir"]).is_dir():
        wwmi_dir = Path(active_ledger["wwmi_dir"])
    elif wwmi_dirs:
        wwmi_dir = wwmi_dirs[0]

    return win64_dir, wwmi_dir, base_root, matched_install


def print_doctor_report(rep: core.DoctorReport) -> None:
    icons = {"ok": "[OK]  ", "warn": "[WARN]", "error": "[ERR] "}
    print(core.tr("=== WWMI-DX12-RT 环境与健康自检报告 ===", "=== WWMI-DX12-RT Environment & Health Check ==="))
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
        print("未自动检测到《鸣潮》安装目录，请通过 --game-win64 指定路径。", file=sys.stderr)
        return 2
    if not wwmi_dir:
        wwmi_dir = base_root / "WWMI"
    rep = core.run_doctor(
        win64_dir=win64_dir,
        wwmi_dir=wwmi_dir,
        base_root=base_root,
        compatdata_dir=inst.compatdata_dir if inst else None,
        is_flatpak_steam=inst.is_flatpak_steam if inst else False,
    )
    if args.json:
        print(json.dumps({"healthy": rep.healthy, **rep.__dict__, "findings": [f.__dict__ for f in rep.findings]}, indent=2, ensure_ascii=False))
    else:
        print_doctor_report(rep)
    return 0 if rep.healthy else 1


def cmd_init_wwmi(args: argparse.Namespace) -> int:
    target = Path(args.target).expanduser().resolve()
    out = core.init_wwmi_skeleton(target)
    print(f"已初始化标准 WWMI 目录骨架：{out}")
    print(f"  - 请将官方 WWMI-Package 的 Core/ 与 RabbitFX 放入该目录，角色皮肤解压至 {out / 'Mods'}")
    return 0


def cmd_install(args: argparse.Namespace) -> int:
    win64_dir, wwmi_dir, base_root, _ = resolve_defaults(args)
    if not win64_dir:
        print("错误：未找到《鸣潮》Win64 目录，请使用 --game-win64 指定。", file=sys.stderr)
        return 2
    dry_run = getattr(args, "dry_run", False)
    init_wwmi = not wwmi_dir and getattr(args, "init_missing_wwmi", False)
    if not wwmi_dir:
        if not init_wwmi:
            print("错误：未找到现有 WWMI 目录，请使用 --wwmi-dir 指定或添加 --init-missing-wwmi。", file=sys.stderr)
            return 2
        wwmi_dir = base_root / "WWMI"

    res = core.install_mount(
        source_root=REPO_ROOT,
        win64_dir=win64_dir,
        wwmi_dir=wwmi_dir,
        base_root=base_root,
        skin_mode=getattr(args, "skin_mode", "default"),
        proton_utf8_guard=getattr(args, "proton_utf8_guard", False),
        dry_run=dry_run,
        init_missing_wwmi=init_wwmi,
    )
    if dry_run:
        print(core.tr("[dry-run] 安装计划校验通过（未写入任何文件）：", "[dry-run] Install plan verified (no files written):"))
        print(json.dumps(res, indent=2, ensure_ascii=False))
    else:
        print(core.tr("安装启用完成！", "Installed and enabled!"))
        print(f"  - {core.tr('外部程序根', 'External root')}: {res['base_root']}")
        print(f"  - {core.tr('关联 Mod 树', 'WWMI tree')}: {res['wwmi_dir']}")
        print(f"  - {core.tr('游戏引导桥', 'Game Win64')}: {res['win64_dir']} ({core.tr('仅 dxgi.dll + ReShade.ini', 'dxgi.dll + ReShade.ini only')})")
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
        print("[dry-run] 卸载预览：")
        print(json.dumps(res, indent=2, ensure_ascii=False))
        return 0, "卸载预检完成，未修改文件。"
    if res.get("state") == "noop":
        msg = res["message"]
        print(msg)
        return 0, msg
    lines = [f"已安全卸载还原游戏目录：{res.get('win64_dir')}"]
    if res.get("staged_batch"):
        lines.append(f"移除的引导文件已安全暂存至：{res['staged_batch']}")
    if res.get("has_legacy_settings"):
        lines.append("旧版账本中的 compatdata 设置已保留；请按需人工检查旧配置和 Steam 启动选项。")
    msg = "\n".join(lines)
    print(msg)
    return 0, msg


def cmd_uninstall(args: argparse.Namespace) -> int:
    rc, _ = _run_uninstall(args)
    return rc


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
        "Name=WWMI DX12+RT 配置器\n"
        "Comment=鸣潮 DX12 + 光追 Mod 外部挂载安装与卸载工具\n"
        f'Exec="{sys.executable}" "{Path(__file__).resolve()}" gui\n'
        "Icon=applications-games\n"
        "Terminal=false\n"
        "Categories=Game;Utility;\n"
    )
    desktop_file.write_text(content, encoding="utf-8")
    desktop_file.chmod(0o755)
    print(f"已生成桌面应用入口：{desktop_file}")
    return 0


def gui_install(args: argparse.Namespace, base_root: Path) -> tuple[int, str]:
    args.init_missing_wwmi = True
    rc = cmd_install(args)
    if rc != 0:
        return rc, "安装未完成：未找到游戏或 WWMI 目录，请查看终端输出。"
    if args.dry_run:
        return rc, "安装预检通过，未写入文件。"
    message = "安装启用成功。"
    _, wwmi_dir, resolved_base, _ = resolve_defaults(args)
    target_wwmi = wwmi_dir or (resolved_base / "WWMI")
    if not (target_wwmi / "Core/WWMI/WuWa-Model-Importer.ini").is_file():
        message += f"\n\n[提示] 尚未检测到官方 Core/WWMI 核心包，请将其放入：\n{target_wwmi}"
    if sys.platform != "win32":
        options, warning = core.proton_launch_options(base_root, args.proton_utf8_guard)
        message += f"\n\nSteam → 游戏属性 → 启动选项（请确保使用 DX12 启动）：\n{options}"
        if warning:
            message += f"\n\n{warning}"
    return rc, message


def cmd_gui(args: argparse.Namespace) -> int:
    """Interactive wizard via kdialog / zenity (for Linux desktop & Steam Deck) with TUI fallback."""
    win64_dir, wwmi_dir, base_root, inst = resolve_defaults(args)
    state, state_msg = (
        core.inspect_mount_state(win64_dir, base_root) if win64_dir else ("not_installed", "未检测到游戏目录")
    )
    summary = (
        f"当前状态：{state} ({state_msg})\n"
        f"游戏目录：{win64_dir or '未检测到'}\n"
        f"WWMI 目录：{wwmi_dir or '未检测到 (安装时将初始化)'}\n"
        f"外部挂载根：{base_root}"
    )

    kdialog = shutil.which("kdialog")
    zenity = shutil.which("zenity")

    if kdialog:
        choice = subprocess.run(
            [
                kdialog,
                "--title",
                "WWMI DX12+RT 安装配置器",
                "--menu",
                f"{summary}\n\n请选择要执行的操作：",
                "install",
                "一键安装启用（原习惯从 Steam / 官方启动器直启）",
                "uninstall",
                "一键卸载还原（恢复 100% 纯净游戏目录）",
                "doctor",
                "运行环境与 RabbitFX 健康自检",
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
            if act == "uninstall":
                args.dry_run = False
                rc, msg = _run_uninstall(args)
                subprocess.run([kdialog, "--msgbox", msg])
                return rc
            if act == "doctor" and win64_dir:
                rep = core.run_doctor(win64_dir, wwmi_dir or (base_root / "WWMI"), base_root,
                                      compatdata_dir=inst.compatdata_dir if inst else None,
                                      is_flatpak_steam=inst.is_flatpak_steam if inst else False)
                msg = "\n".join(f"[{f.level.upper()}] {f.message}" for f in rep.findings)
                subprocess.run([kdialog, "--msgbox", msg])
                return 0
        except Exception as exc:
            subprocess.run([kdialog, "--error", str(exc)])
            return 1

    elif zenity:
        choice = subprocess.run(
            [
                zenity,
                "--list",
                "--title=WWMI DX12+RT 安装配置器",
                f"--text={summary}",
                "--column=代码",
                "--column=操作说明",
                "install",
                "一键安装启用（原习惯从 Steam / 官方启动器直启）",
                "uninstall",
                "一键卸载还原（恢复 100% 纯净游戏目录）",
                "doctor",
                "运行环境与 RabbitFX 健康自检",
                "--width=680",
                "--height=360",
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
            if act == "uninstall":
                args.dry_run = False
                rc, msg = _run_uninstall(args)
                subprocess.run([zenity, "--info", f"--text={msg}"])
                return rc
            if act == "doctor" and win64_dir:
                rep = core.run_doctor(win64_dir, wwmi_dir or (base_root / "WWMI"), base_root,
                                      compatdata_dir=inst.compatdata_dir if inst else None,
                                      is_flatpak_steam=inst.is_flatpak_steam if inst else False)
                msg = "\n".join(f"[{f.level.upper()}] {f.message}" for f in rep.findings)
                subprocess.run([zenity, "--info", f"--text={msg}"])
                return 0
        except Exception as exc:
            subprocess.run([zenity, "--error", f"--text={exc}"])
            return 1

    # Terminal interactive fallback
    print(summary)
    print("\n1) 一键安装启用   2) 一键卸载还原   3) 健康自检   q) 退出")
    sel = input("请输入选项 [1/2/3/q]: ").strip().lower()
    if sel == "1":
        rc, _ = gui_install(args, base_root)
        return rc
    if sel == "2":
        args.dry_run = False
        return cmd_uninstall(args)
    if sel == "3":
        args.json = False
        return cmd_doctor(args)
    return 0


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(description="WWMI-DX12-RT cross-platform external mount installer & doctor")
    p.add_argument("--game-win64", help="Wuthering Waves root or Client/Binaries/Win64 path (auto-detected by default)")
    p.add_argument("--wwmi-dir", help="WWMI directory containing d3dx.ini and Mods/ (auto-detected by default)")
    p.add_argument("--base-root", help="External runtime/cache root (default ~/.local/share/wwmi-dx12-rt on Linux)")
    p.add_argument("--lang", choices=("zh", "en"), help="Override UI/output language (zh or en)")

    shared = argparse.ArgumentParser(add_help=False)
    shared.add_argument("--game-win64", default=argparse.SUPPRESS)
    shared.add_argument("--wwmi-dir", default=argparse.SUPPRESS)
    shared.add_argument("--base-root", default=argparse.SUPPRESS)
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

    p_init = sub.add_parser("init-wwmi", parents=[shared], help="Initialize a standard WWMI folder skeleton")
    p_init.add_argument("target", help="Target directory path")

    p_gui = sub.add_parser("gui", parents=[shared], help="Launch interactive GUI wizard (kdialog/zenity) or TUI")
    add_install_options(p_gui, "default")
    sub.add_parser("desktop-entry", parents=[shared], help="Create a desktop menu entry in ~/.local/share/applications")
    return p


def main(argv: list[str] | None = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
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
    if cmd == "init-wwmi":
        return cmd_init_wwmi(args)
    if cmd == "gui":
        return cmd_gui(args)
    if cmd == "desktop-entry":
        return cmd_desktop_entry(args)
    parser.print_help()
    return 64


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Exception as exc:
        print(f"错误：{exc}", file=sys.stderr)
        sys.exit(1)
