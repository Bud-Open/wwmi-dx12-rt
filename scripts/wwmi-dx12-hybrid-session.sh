#!/usr/bin/env bash
# 独立混合绘制候选。沿用 06 的锁、Proton 参数、生产部署账本和退出恢复。
# 只允许本候选 payload；只写普通绘制摘要，不启动 debug、UAV/Shape 观测或采集。
set -uo pipefail
# 与原版 Steam 启动项相同，仅屏蔽本次游戏进程树的输入法。
export XMODIFIERS=@im=none
export SDL_IM_MODULE=none
export GTK_IM_MODULE=gtk-im-context-simple
export QT_IM_MODULE=compose

APPID=3513350
MODE="run"
SKIN="exact"
HERE="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd -P)"
PLAY="${HERE%/*}/play.sh"
# 本包只替换含绘制摘要的主 DLL，沿用已验证的 Wine 文件名编码兼容库。
PAYLOAD="$HERE/../payload"
WINE_FILENAME_UTF8="${HERE%/*}/lib/wine-filename-utf8.so"
WWMI_TREE="${WWMI_DX12_TREE:-$HOME/wwmi-stage5-wwmi-tree}"
SCRIPTS="$HERE/runtime"
DEPLOY="$SCRIPTS/wwmi-stage5-native-host-deploy.py"
RECOVER="$SCRIPTS/wwmi-stage5-native-host-recover.py"
STATE_ROOT="${WWMI_DX12_STATE:-$HOME/.local/state/wwmi-dx12}"
STEAM_GAME_DIR="$HOME/.local/share/Steam/steamapps/common/Wuthering Waves"
DEFAULT_GAME_DIR="$STEAM_GAME_DIR/Wuthering Waves Game"
GAME_DIR="${WWMI_DX12_GAME_DIR:-$DEFAULT_GAME_DIR}"
WIN64="${WWMI_DX12_WIN64:-$GAME_DIR/Client/Binaries/Win64}"
GAME_EXE="${WWMI_DX12_GAME_EXE:-$GAME_DIR/Wuthering Waves.exe}"
SHIPPING_EXE="${WWMI_DX12_SHIPPING_EXE:-$WIN64/Client-Win64-Shipping.exe}"
GAME_LOG="${WWMI_DX12_GAME_LOG:-$GAME_DIR/Client/Saved/Logs/Client.log}"
OFFICIAL_LAUNCHER_DIR="${WWMI_DX12_LAUNCHER_DIR:-$STEAM_GAME_DIR}"
PROCESS_PREFIX="$HOME/.local/share/Steam/steamapps/compatdata/$APPID/pfx"
# 自动适配本地客户端已安装的基础画质包档位（存在 Client/Content/UHD 则必须使用 uhd，绝不强切 hd 触发额外下载或缺失基础 UI 包崩溃）。
if [[ -n "${WWMI_DX12_RESOURCE_QUALITY:-}" ]]; then
    RESOURCE_QUALITY="$WWMI_DX12_RESOURCE_QUALITY"
elif [[ -d "$GAME_DIR/Client/Content/UHD" ]]; then
    RESOURCE_QUALITY="uhd"
else
    RESOURCE_QUALITY="hd"
fi
GAME_LAUNCH_ARGS=("-krqlv=$RESOURCE_QUALITY")

while [[ $# -gt 0 ]]; do
    case "$1" in
        --dry-run) MODE="dry-run" ;;
        --recover) MODE="recover" ;;
        --base-skin) SKIN="base" ;;
        -h|--help)
            printf '用法：%s [--base-skin] [--dry-run|--recover]\n' "${PLAY##*/}"
            printf '%s\n' '默认：缺准确形变的原生 draw 保留游戏外观。' '--base-skin：第三方全外观－基础蒙皮；保留真实骨骼，可能省略额外表情。'
            exit 0 ;;
        *) printf '未知参数：%s\n' "$1" >&2; exit 64 ;;
    esac
    shift
done

mkdir -p "$STATE_ROOT" || { printf '无法创建状态目录 %s\n' "$STATE_ROOT" >&2; exit 1; }
STAMP="$(date +%Y%m%d-%H%M%S)"
RUN_DIR="$STATE_ROOT/run-$STAMP"
# --dry-run 与 --recover 都不建会话目录，避免留下空壳。
[[ "$MODE" == "run" ]] && { mkdir -p "$RUN_DIR" || exit 1; }
LOG="$RUN_DIR/session.log"

log() {
    local line
    line="$(date '+%F %T') $*"
    printf '%s\n' "$line"
    [[ "$MODE" == "run" ]] && printf '%s\n' "$line" >>"$LOG"
    return 0
}
die() { log "中止：$*"; exit 1; }

# 会话锁：正常运行与 --recover 共用同一把，避免双击并发搬同一批文件。
exec 9>"$STATE_ROOT/session.lock" || die "无法打开会话锁"
if ! flock -n 9; then
    die "另一个 Mod 会话正在进行（$STATE_ROOT/session.lock）。等它结束后再试。"
fi

# 保留原游戏/XXMI 检查；新增官方启动器只认本安装路径，不按泛名全局拦截。
conflicting_processes() {
    # 一次 Python 扫描代替每个 PID 启动 tr/head/子 shell；输出契约不变。
    python3 - "$OFFICIAL_LAUNCHER_DIR" "$PROCESS_PREFIX" <<'PROC_PY'
from pathlib import Path
import sys
names = {b"Client-Win64-Shipping.exe", b"Wuthering Waves.exe", b"XXMI Launcher.exe"}
launcher_names = {b"launcher.exe", b"launcher_main.exe", b"launcher_updater.exe"}
launcher_root = Path(sys.argv[1]).resolve()
prefix = Path(sys.argv[2])

def launcher_path(argv0, pid):
    name = argv0.decode("utf-8", "replace").replace("\\", "/")
    if len(name) >= 2 and name[1] == ":":
        return (prefix / "dosdevices" / name[:2].lower() / name[2:].lstrip("/")).resolve()
    path = Path(name)
    if not path.is_absolute():
        path = pid / "cwd" / path
    return path.resolve()

hits = []
for pid in sorted(Path("/proc").glob("[0-9]*")):
    try:
        argv0 = (pid / "cmdline").read_bytes().split(b"\0", 1)[0].split(b"\n", 1)[0]
    except OSError:
        continue
    base = argv0.replace(b"\\", b"/").rsplit(b"/", 1)[-1]
    watched = base in names
    if base.lower() in launcher_names:
        try:
            watched = launcher_path(argv0, pid).is_relative_to(launcher_root)
        except OSError:
            continue
    if watched:
        hits.append(f"{pid.name}:{base.decode('utf-8', 'replace')} ")
sys.stdout.write("".join(hits))
PROC_PY
}

# 只认可「本入口自己写在 STATE_ROOT 下、且游戏目录与本会话一致」的账本。
own_unrecovered_ledgers() {
    local results
    for results in "$STATE_ROOT"/run-*/deploy; do
        [[ -f "$results/deploy-manifest.json" ]] || continue
        python3 - "$results/deploy-manifest.json" "$WIN64" <<'PY' || continue
import json, sys
from pathlib import Path
record = json.load(open(sys.argv[1], encoding="utf-8"))
if (record.get("recovery") or {}).get("state") == "recovered":
    raise SystemExit(1)
if Path(record.get("game_win64", "")).resolve() != Path(sys.argv[2]).resolve():
    raise SystemExit(1)
print(Path(sys.argv[1]).parent)
PY
    done
}

recover_one() {
    local results="$1" label="$2"
    log "撤回 $label：$results"
    local extra=()
    [[ -n "${WWMI_DX12_STAGING_ROOT:-}" ]] && extra+=(--staging-root "$WWMI_DX12_STAGING_ROOT")
    if python3 "$RECOVER" --results "$results" \
        --batch "batch-$(date +%Y%m%d-%H%M%S)-wwmi-dx12-mod" "${extra[@]}"; then
        return 0
    fi
    log "撤回失败：$results"
    return 1
}

# 收尾：游戏仍在时不移动任何文件，只留下待恢复状态。
finish() {
    local rc="$1" reason="$2"
    (( SIGNALED )) && reason="signaled-$reason"
    log "会话结束（rc=$rc，原因：$reason）"

    local running
    running="$(conflicting_processes)"
    if [[ -n "$running" ]]; then
        log "检测到游戏/XXMI 或本游戏官方启动器仍在运行（$running）：不移动注入文件。"
        log "待恢复账本：$RUN_DIR/deploy"
        printf '%s\n' "reason=$reason rc=$rc running=$running" >"$RUN_DIR/PENDING-RECOVERY" 2>/dev/null || true
        log "游戏及官方启动器退出后运行：$PLAY --recover"
        exit "$(( rc == 0 ? 3 : rc ))"
    fi

    if [[ ! -f "$RUN_DIR/deploy/deploy-manifest.json" ]]; then
        log "本次没有部署账本，无需撤回"
        exit "$rc"
    fi
    if recover_one "$RUN_DIR/deploy" "本次会话"; then
        log "撤回完成"
        exit "$rc"
    fi
    printf '%s\n' "reason=recover-failed rc=$rc" >"$RUN_DIR/PENDING-RECOVERY" 2>/dev/null || true
    log "撤回失败，注入文件可能仍在游戏目录。账本：$RUN_DIR/deploy"
    log "排除错误后，用同一入口重试：$PLAY --recover"
    exit "$(( rc == 0 ? 4 : rc ))"
}

# 信号：不杀游戏、不移动仍在使用的文件；等游戏自己结束后再走正常收尾。
SIGNALED=0
on_signal() {
    SIGNALED=1
    log "收到信号 $1：不移动正在使用的文件，等待游戏退出"
}
trap 'on_signal INT'  INT
trap 'on_signal TERM' TERM
trap 'on_signal HUP'  HUP

# ---------- --recover ----------
if [[ "$MODE" == "recover" ]]; then
    running="$(conflicting_processes)"
    [[ -z "$running" ]] || die "游戏/XXMI 或本游戏官方启动器仍在运行（$running）：下载完成后正常关闭，再撤回；本入口不终止它。"
    mapfile -t pending < <(own_unrecovered_ledgers)
    if [[ ${#pending[@]} -eq 0 ]]; then
        log "没有待撤回的自有账本（可能已撤回）"
        exit 0
    fi
    rc=0
    for results in "${pending[@]}"; do
        recover_one "$results" "残留" || rc=1
    done
    exit "$rc"
fi

# ---------- 前置检查 ----------
log "前置检查开始"
running="$(conflicting_processes)"
if [[ -n "$running" ]]; then
    die "游戏/XXMI 或本游戏官方启动器已在运行（$running）。下载完成后请正常关闭官方启动器，再从 Steam 的 DX12 Mod 条目启动；本入口不终止它。"
fi
[[ -d "$PAYLOAD/artifacts/stage5" ]] || die "持久 payload 缺失：$PAYLOAD"
[[ -r "$WINE_FILENAME_UTF8" ]] || die "Wine 文件名编码库不可读：$WINE_FILENAME_UTF8"
[[ -f "$WWMI_TREE/d3dx.ini" ]] || die "隔离 WWMI 树缺少 d3dx.ini：$WWMI_TREE"
[[ -f "$GAME_EXE" ]] || die "游戏主程序不存在：$GAME_EXE"
python3 -c 'import protontricks' 2>/dev/null || die "缺少 protontricks，无法解析 Proton"

log "前置检查完成"
log "独立混合绘制 payload：$PAYLOAD（策略：$SKIN）"
log "隔离树：$WWMI_TREE"
log "游戏根：$GAME_DIR"
log "启动目标：$GAME_EXE（-krqlv=$RESOURCE_QUALITY）"
log "部署目录：$WIN64"
log "游戏配置与日志来源：$GAME_DIR/Client/Saved；旧目录不迁移"

DEPLOY_EXTRA=(--win64 "$WIN64")

# ---------- dry-run：只报告，不搬任何文件 ----------
if [[ "$MODE" == "dry-run" ]]; then
    mapfile -t planned < <(own_unrecovered_ledgers)
    if [[ ${#planned[@]} -gt 0 ]]; then
        log "[dry-run] 有 ${#planned[@]} 个未撤回的自有账本，正式运行时会先恢复（本次不搬）："
        for results in "${planned[@]}"; do
            log "[dry-run]   待恢复 $results"
        done
    else
        log "[dry-run] 没有未撤回的自有账本"
    fi
    for rel in dxgi.dll ReShade.ini wwmi_dx12.ini reshade-addons; do
        [[ -e "$WIN64/$rel" ]] && log "[dry-run] 游戏目录已存在 $rel"
    done
    log "[dry-run] 将部署独立混合绘制候选到 $WIN64（项目以部署计划为准）"
    python3 "$DEPLOY" --package "$PAYLOAD" --results "$RUN_DIR/deploy" \
        --profile candidate --wwmi-tree "$WWMI_TREE" --dry-run "${DEPLOY_EXTRA[@]}"
    dry_rc=$?
    if (( dry_rc != 0 )); then
        log "[dry-run] 部署前置检查未通过（rc=$dry_rc），正式运行同样会被拒绝"
        exit "$dry_rc"
    fi
    log "[dry-run] 之后沿原 Steam/Proton 链启动：$GAME_EXE -krqlv=$RESOURCE_QUALITY（不带 -dx11）"
    exit 0
fi

# ---------- 正式运行：先恢复自己的残留，再部署 ----------
mapfile -t pending < <(own_unrecovered_ledgers)
if [[ ${#pending[@]} -gt 0 ]]; then
    log "发现 ${#pending[@]} 个未撤回的自有账本，先恢复"
    for results in "${pending[@]}"; do
        recover_one "$results" "上次残留" || die "上次残留恢复失败，本次不部署"
    done
fi

# 恢复后仍有同名文件 = 不属于任何自有账本，拒绝并交人工核对。
for rel in dxgi.dll ReShade.ini wwmi_dx12.ini reshade-addons; do
    if [[ -e "$WIN64/$rel" ]]; then
        die "游戏目录已有 $rel，且不属于本入口的账本；请人工核对后再试"
    fi
done

# ---------- 部署 ----------
if ! python3 "$DEPLOY" --package "$PAYLOAD" --results "$RUN_DIR/deploy" \
        --profile candidate --wwmi-tree "$WWMI_TREE" "${DEPLOY_EXTRA[@]}" >>"$LOG" 2>&1; then
    log "部署失败，日志：$LOG"
    finish 1 "deploy-failed"
fi
log "部署完成，账本：$RUN_DIR/deploy/deploy-manifest.json"

# ---------- 解析 Proton / Runtime ----------
resolved="$(python3 - <<'PY'
import sys
from protontricks.steam import (
    find_steam_installations, find_proton_app, get_steam_apps, get_steam_lib_paths)

appid = 3513350
steam_path, steam_root = find_steam_installations()[0]
libs = get_steam_lib_paths(steam_path)
apps = get_steam_apps(steam_root=steam_root, steam_path=steam_path, steam_lib_paths=libs)
proton = find_proton_app(steam_path=steam_path, steam_apps=apps, appid=appid)
if proton is None or proton.required_tool_app is None:
    sys.exit("无法解析 Proton 或 Steam Runtime")
print(proton.install_path)
print(proton.required_tool_app.install_path)
print(steam_path)
PY
)" || { log "Proton 解析失败，日志：$LOG"; finish 1 "proton-resolve-failed"; }
mapfile -t parts <<<"$resolved"
[[ ${#parts[@]} -eq 3 ]] || { log "Proton 解析结果不完整"; finish 1 "proton-resolve-incomplete"; }
PROTON_DIR="${parts[0]}"; RUNTIME_DIR="${parts[1]}"; STEAM_DIR="${parts[2]}"
COMPAT_DATA="$STEAM_DIR/steamapps/compatdata/$APPID"
RUNTIME_ENTRY="$RUNTIME_DIR/_v2-entry-point"
PROTON_BIN="$PROTON_DIR/proton"
WINESERVER_BIN="$PROTON_DIR/files/bin/wineserver"
for required in "$RUNTIME_ENTRY" "$PROTON_BIN" "$WINESERVER_BIN" "$COMPAT_DATA/pfx"; do
    [[ -e "$required" ]] || { log "缺少必需路径：$required"; finish 1 "missing-runtime-path"; }
done

# ---------- 环境：与已验证 launch.sh 一致，只影响本进程树 ----------
export RESHADE_DISABLE_LOGGING=1
# 原生 V2 档案与贴图/着色器运行缓存归持久状态根，不属于单次注入/退出回收清单。
native_profile_root="$(cd -- "$STATE_ROOT" && pwd -P)"
export WWMI_RT_NATIVE_PROFILE_CACHE="Z:${native_profile_root//\//\\}\\native-profiles-v2.txt"
persistent_cache_dir="$native_profile_root/wwmi-runtime/cache"
persistent_seed_dir="$native_profile_root/wwmi-runtime/seed"
mkdir -p "$persistent_cache_dir/shaders" "$persistent_seed_dir"
payload_abs="$(cd -- "$PAYLOAD" && pwd -P)"
seed_dirs_win=""
if [[ -d "$payload_abs/seed" ]]; then
    seed_dirs_win="Z:${payload_abs//\//\\}\\seed"
fi
user_seed_win="Z:${persistent_seed_dir//\//\\}"
if [[ -n "$seed_dirs_win" ]]; then
    seed_dirs_win="${seed_dirs_win};${user_seed_win}"
else
    seed_dirs_win="${user_seed_win}"
fi
export WWMI_DX12_SEED_DIRS="$seed_dirs_win"
export WWMI_DX12_RUNTIME_DIR="Z:${native_profile_root//\//\\}\\wwmi-runtime"
export WWMI_HYBRID_RASTER_MODE="$SKIN"
# 继承的变量名虽为 CAPTURE_DIR，用于存放本轮会话独立日志，同时保持回调注册激活。
runtime_dir="$(cd -- "$RUN_DIR/deploy" && pwd -P)" || finish 1 "runtime-dir-unavailable"
export WWMI_RT_CAPTURE_DIR="Z:${runtime_dir//\//\\}"
# 不提供观察令牌，不开 Material CHS/场景快照或 Shape 采集。
unset WWMI_RT_CAPTURE_SESSION WWMI_RT_SHAPE_EVIDENCE WWMI_RT_SHAPE_EVIDENCE_DIR
export LANG=zh_CN.UTF-8 LC_ALL=zh_CN.UTF-8 HOST_LC_ALL=zh_CN.UTF-8
export STEAM_COMPAT_CLIENT_INSTALL_PATH="$STEAM_DIR"
export STEAM_COMPAT_DATA_PATH="$COMPAT_DATA"
export STEAM_COMPAT_TOOL_PATHS="$PROTON_DIR:$RUNTIME_DIR"
export STEAM_COMPAT_APP_ID="$APPID" SteamAppId="$APPID" SteamGameId="$APPID"
export SteamOS=1
export MANGOHUD=1
export MANGOHUD_CONFIG="fps_only,toggle_hud=Shift_R+F12"
export PROTON_ENABLE_NGX_UPDATER=1
export DXVK_NVAPI_DRS_SETTINGS="NGX_DLSS_FG_OVERRIDE=on,NGX_DLSSG_MODE=dynamic,NGX_DLSSG_DYNAMIC_TARGET_FRAME_RATE=240,NGX_DLSSG_DYNAMIC_MULTI_FRAME_COUNT_MAX=5"

if [[ "${SteamClientLaunch:-}" == "1" && -n "${LD_PRELOAD:-}" ]]; then
    kept=""; dropped=0
    IFS=': ' read -r -a entries <<<"$LD_PRELOAD"
    for entry in "${entries[@]}"; do
        [[ -n "$entry" ]] || continue
        if [[ "$(basename -- "$entry")" == "gameoverlayrenderer.so" ]]; then
            dropped=$((dropped + 1)); continue
        fi
        kept="${kept:+$kept:}$entry"
    done
    if (( dropped > 0 )); then
        if [[ -n "$kept" ]]; then export LD_PRELOAD="$kept"; else unset LD_PRELOAD; fi
        log "LD_PRELOAD：已移除 overlay 预加载项 $dropped 个"
    fi
fi

# 传给容器内 shell 的路径
export WWMI_GAME="$GAME_EXE"
export WWMI_PROTON="$PROTON_BIN"
export WWMI_WINESERVER="$WINESERVER_BIN"
export WWMI_PREFIX="$COMPAT_DATA/pfx"
export WWMI_WINE_FILENAME_UTF8="$WINE_FILENAME_UTF8"

log "启动：proton=$(basename "$PROTON_DIR") runtime=$(basename "$RUNTIME_DIR") prefix=$APPID"

# 自动同步 IMM 已启用的皮肤软链到隔离 Mods 目录（只读扫描 IMM，禁裸删，停用项移入 deletion-staging）
python3 - "$$" "$HOME/.local/share/XXMI-Launcher/WWMI/Mods/DO NOT MODIFY (Managed by IMM)" "$WWMI_TREE/Mods" 9>&- >> "$LOG" 2>&1 <<'IMM_SYNC_PY' &
import datetime, json, os, shutil, sys, time
from pathlib import Path

session_pid = int(sys.argv[1])
imm_root = Path(sys.argv[2])
dx12_mods = Path(sys.argv[3])
staging_root = Path.home() / ".pi" / "deletion-staging"

def sync_once():
    if not imm_root.is_dir() or not dx12_mods.is_dir():
        return
    enabled = {}
    for p in imm_root.glob("*/*"):
        if p.is_symlink():
            target = p.resolve()
            if target.is_dir():
                if any(f.name.lower() == "rabbitfx.ini" for f in target.rglob("*.ini")):
                    continue
                enabled[p.name] = target
    for name, target in enabled.items():
        dst = dx12_mods / name
        if not dst.exists() and not dst.is_symlink():
            dst.symlink_to(target)
            print(f"IMM-SYNC linked: {name} -> {target}", flush=True)
    # If an IMM symlink in dx12_mods was disabled in IMM, stage it away safely (Rule #8)
    to_stage = []
    for item in dx12_mods.iterdir():
        if item.is_symlink():
            t = item.resolve()
            if "IMM-Mods" in str(t) and enabled.get(item.name) != t:
                to_stage.append(item)
    if to_stage:
        staging_root.mkdir(mode=0o700, parents=True, exist_ok=True)
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        batch = staging_root / f"batch-{stamp}-imm-mod-disable"
        batch.mkdir(mode=0o700, exist_ok=True)
        entries = []
        for idx, item in enumerate(to_stage):
            staged_path = batch / f"{idx:03d}_{item.name}"
            shutil.move(str(item), str(staged_path))
            entries.append({"original_path": str(item), "staged_path": str(staged_path), "kind": "symlink"})
            print(f"IMM-SYNC staged disabled symlink: {item.name} -> {staged_path}", flush=True)
        (batch / "manifest.json").write_text(json.dumps({"created_at": stamp, "entries": entries}, indent=2, ensure_ascii=False))

sync_once()
while True:
    time.sleep(2.0)
    try:
        os.kill(session_pid, 0)
    except ProcessLookupError:
        break
    try:
        sync_once()
    except Exception as exc:
        print(f"IMM-SYNC warn: {exc}", flush=True)
IMM_SYNC_PY

# 与 launch.sh 相同的等待结构：proton waitforexitandrun 之后，再在同前缀等 wineserver 结束，
# 避免启动器一返回就撤回游戏仍在使用的文件。
python3 "$SCRIPTS/finish-exited-client.py" --session-pid "$$" \
    --game-exe "$SHIPPING_EXE" --log "$GAME_LOG" \
    --events "$RUN_DIR/confirmed-exit.jsonl" 9>&- >> "$LOG" 2>&1 &
"$RUNTIME_ENTRY" --verb=waitforexitandrun -- /bin/sh -c '
    if [ ! -r "$WWMI_WINE_FILENAME_UTF8" ]; then
        printf "容器内 Wine 文件名编码库不可读：%s\n" "$WWMI_WINE_FILENAME_UTF8" >&2
        exit 1
    fi
    LD_PRELOAD="$WWMI_WINE_FILENAME_UTF8${LD_PRELOAD:+:$LD_PRELOAD}" \
        "$WWMI_PROTON" waitforexitandrun "$WWMI_GAME" "$@"
    game_rc=$?
    if [ "$game_rc" -ne 0 ]; then
        exit "$game_rc"
    fi
    WINEPREFIX="$WWMI_PREFIX" "$WWMI_WINESERVER" -w
    exit $?
' wwmi-session "${GAME_LAUNCH_ARGS[@]}"
game_rc=$?
log "容器会话返回 rc=$game_rc"

finish "$game_rc" "game-exit"
