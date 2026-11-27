"""Exercise the shipped Win32 Setup against a simulated game. Fixtures are retained."""
from __future__ import annotations

import argparse
import ctypes
import json
import os
import subprocess
import tempfile
import zipfile
from pathlib import Path


SHIPPING = "Client-Win64-Shipping.exe"


def smoke(archive: Path, root: Path, wine: str | None = None) -> None:
    def engine_path(path: Path) -> str:
        return "Z:" + path.as_posix().replace("/", "\\") if wine else str(path)

    def game(name: str) -> Path:
        out = root / name / "Client/Binaries/Win64"
        out.mkdir(parents=True)
        (out / SHIPPING).touch()
        return out

    with zipfile.ZipFile(archive) as zip_file:
        zip_file.extractall(root)
    base = root / "WWMI-DX12-RT-win64"
    exe = base / "WWMI-DX12-RT-Setup.exe"
    assert exe.is_file(), exe
    win64 = game("GameA")
    other = game("GameB")
    wwmi = root / "中文 WWMI"
    journal = base / "runtime/install-ledger.json"

    def cli(action: str, target: Path = win64, *, rc: int = 0, reason: str = "", lang: str = "en") -> None:
        args = ([wine] if wine else []) + [
            str(exe), "--lang", lang, "--cli", action, "--game", engine_path(target), "--wwmi", engine_path(wwmi),
        ]
        result = subprocess.run(args, cwd=base, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=45)
        output = result.stdout.decode("utf-8", errors="replace")
        print(output, end="", flush=True)
        assert result.returncode == rc, (action, result.returncode, output)
        assert reason in output, (reason, output)

    cli("init-wwmi")
    assert (wwmi / "d3dx.ini").is_file()
    d3dx = (wwmi / "d3dx.ini").read_bytes()
    (win64 / "renodx-sample.addon64").write_bytes(b"ADDON")
    cli("install", reason="renodx-sample.addon64", lang="en")
    assert {p.name for p in win64.iterdir()} == {SHIPPING, "dxgi.dll", "ReShade.ini", "renodx-sample.addon64"}
    ext_ini = base / "ReShade.ini"
    ext_ini.write_text(
        "[INSTALL]\r\nLogging=1\r\n\r\n[ADDON]\r\nAddonPath=D:\\Extra\r\nLoadGameDirAddons=1\r\n",
        encoding="utf-8",
    )
    cli("install", lang="zh", reason="安装启用成功")
    merged = ext_ini.read_text(encoding="utf-8")
    assert "Logging=1" in merged and "LoadGameDirAddons=1" in merged and "AddonPath=reshade-addons,D:\\Extra" in merged, merged
    cli("doctor", reason="Other ReShade add-on(s) in Win64", lang="en")
    cli("doctor", reason="Win64 存在其他 ReShade 插件", lang="zh")
    ini = base / "wwmi_dx12.ini"
    data = ini.read_bytes()
    assert data.startswith(b"\xff\xfe")
    text = data[2:].decode("utf-16-le")
    assert "\n" not in text.replace("\r\n", "")
    if wine:
        assert f"wwmi_dir={engine_path(wwmi)}\r\n" in text
    else:
        read = ctypes.WinDLL("kernel32", use_last_error=True).GetPrivateProfileStringW
        read.argtypes = [ctypes.c_wchar_p] * 4 + [ctypes.c_uint32, ctypes.c_wchar_p]
        read.restype = ctypes.c_uint32
        out = ctypes.create_unicode_buffer(32768)
        assert read("wwmi-dx12", "wwmi_dir", "", out, len(out), str(ini))
        assert Path(out.value) == wwmi

    installed = journal.read_bytes()
    cli("install", other, rc=1, reason="Uninstall the previous", lang="en")
    cli("install", other, rc=1, reason="先卸载旧游戏目录", lang="zh")
    assert {p.name for p in other.iterdir()} == {SHIPPING}
    assert journal.read_bytes() == installed

    stub = win64 / "ReShade.ini"
    original = stub.read_bytes()
    stub.write_bytes(b"foreign")
    cli("install", rc=1, reason="hash does not match", lang="en")
    cli("install", rc=1, reason="哈希", lang="zh")
    assert stub.read_bytes() == b"foreign"
    assert journal.read_bytes() == installed
    stub.write_bytes(original)

    journal.write_bytes(installed + b"invalid-json")
    cli("uninstall", rc=1, reason="ledger is corrupted", lang="en")
    cli("uninstall", rc=1, reason="账本损坏", lang="zh")
    assert journal.read_bytes() == installed + b"invalid-json"
    journal.write_bytes(installed)

    cli("uninstall", lang="en", reason="Uninstalled")
    assert {p.name for p in win64.iterdir()} == {SHIPPING, "renodx-sample.addon64"}
    record = json.loads(journal.read_bytes())
    assert record["state"] == "uninstalled", record
    staged = record["staged_batch"]
    batch = Path(staged[2:].replace("\\", "/") if wine else staged)
    manifest = json.loads((batch / "manifest.json").read_bytes())
    assert len(manifest["entries"]) == 2
    for item in manifest["entries"]:
        assert (batch / item["staged_path"].rsplit("\\", 1)[1]).is_file()
    assert (wwmi / "d3dx.ini").read_bytes() == d3dx
    print(f"Win32 Setup ZIP smoke passed: {root}")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("archive", type=Path)
    parser.add_argument("--wine", help="Run the same smoke test in an isolated Wine prefix on Linux")
    args = parser.parse_args()
    root = Path(tempfile.mkdtemp(prefix="wwmi-win-smoke-")).resolve()
    if args.wine:
        os.environ.update(WINEPREFIX=str(root / "wine"), WINEDEBUG="-all", WINEDLLOVERRIDES="winemenubuilder.exe=d")
    smoke(args.archive.resolve(strict=True), root, args.wine)


if __name__ == "__main__":
    main()
