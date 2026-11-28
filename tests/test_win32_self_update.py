#!/usr/bin/env python3
"""End-to-end Wine CLI tests for Windows WWMI-DX12-RT-Setup.exe slot self-update and rollback."""
from __future__ import annotations

import importlib.util
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "scripts"))

import installer_core as core  # noqa: E402

SETUP_EXE = ROOT / "payload/artifacts/stage5/WWMI-DX12-RT-Setup.exe"


def load_package_release():
    spec = importlib.util.spec_from_file_location("package_release_wsu_test", ROOT / "scripts/package_release.py")
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def populate_win_release_dir(target: Path, version: str, commit: str, host_bytes: bytes) -> Path:
    target.mkdir(parents=True, exist_ok=True)
    for rel in core.REQUIRED_PAYLOAD:
        p = target / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        if rel == "host/ReShade64-game-forward.dll":
            p.write_bytes(host_bytes)
        else:
            p.write_bytes(f"{version}:{rel}".encode("utf-8"))
    (target / "seed").mkdir(parents=True, exist_ok=True)
    (target / "seed/wwmi_dx12_seed.texlook").write_bytes(b"SEED-" + version.encode("utf-8"))
    (target / "BUILD-PROVENANCE.json").write_text(
        json.dumps({"version": version, "repository_commit": commit, "working_tree_dirty": False}, indent=2) + "\n",
        encoding="utf-8",
    )
    return target


def to_wine_path(p: Path) -> str:
    return "Z:" + str(p.resolve()).replace("/", "\\")


@unittest.skipUnless(shutil.which("wine") and SETUP_EXE.is_file(), "Requires wine and built WWMI-DX12-RT-Setup.exe")
class Win32SelfUpdateWineTests(unittest.TestCase):
    def run_setup(self, exe: Path, args: list[str], wineprefix: Path) -> subprocess.CompletedProcess[str]:
        env = dict(os.environ)
        env.pop("WAYLAND_DISPLAY", None)
        env.pop("DISPLAY", None)
        env["WINEDEBUG"] = "-all"
        env["WINEPREFIX"] = str(wineprefix)
        return subprocess.run(
            ["wine", str(exe), *args],
            env=env,
            capture_output=True,
            text=True,
            timeout=60,
        )

    def test_wine_slot_self_update_tamper_reject_prefs_and_rollback(self) -> None:
        pkg = load_package_release()
        with tempfile.TemporaryDirectory(prefix="wwmi-win-su-") as tmp:
            case = Path(tmp)
            wp = case / "wineprefix"
            wp.mkdir()
            win64 = case / "Game/Wuthering Waves Game/Client/Binaries/Win64"
            win64.mkdir(parents=True)
            (win64 / core.TARGET_SHIPPING_EXE).write_bytes(b"MZ_FAKE_GAME")
            wwmi = case / "WWMI"
            core.init_wwmi_skeleton(wwmi)

            base = populate_win_release_dir(case / "InstallRoot", "v1.0.5", "11111111" * 5, b"WIN_HOST_V105")
            exe = base / "WWMI-DX12-RT-Setup.exe"
            shutil.copy2(SETUP_EXE, exe)

            # 1. Initial install v1.0.5
            r_ins = self.run_setup(exe, [
                "--cli", "install", "--lang", "en",
                "--game", to_wine_path(win64),
                "--wwmi", to_wine_path(wwmi),
            ], wp)
            self.assertEqual(r_ins.returncode, 0, r_ins.stdout + r_ins.stderr)
            self.assertEqual((win64 / "dxgi.dll").read_bytes(), b"WIN_HOST_V105")

            # Add user third-party add-on + custom preset in external ReShade.ini
            (base / "reshade-addons/RenoDX.addon64").write_bytes(b"USER_RENODX_WIN")
            ext_rs = base / "ReShade.ini"
            ext_rs.write_text(ext_rs.read_text(encoding="utf-8") + "\r\n[GENERAL]\r\nPresetPath=UserPreset.ini\r\n", encoding="utf-8")
            orig_stub = (win64 / "ReShade.ini").read_bytes()

            # 2. Build real v1.0.6 ZIP + update-manifest.json
            rel6 = populate_win_release_dir(case / "pkg/WWMI-DX12-RT-win64", "v1.0.6", "22222222" * 5, b"WIN_HOST_V106")
            zip6 = case / "WWMI-DX12-RT-win64.zip"
            pkg.make_zip(rel6, zip6)
            manifest6 = pkg.build_update_manifest(
                {"version": "v1.0.6", "repository_commit": "22222222" * 5},
                {"win-x64": pkg.build_asset_manifest_entry(rel6, zip6, "zip")},
            )
            mpath = case / "update-manifest.json"
            pkg.write_update_manifest(manifest6, mpath)

            # 3. Apply verified local update via Win32 Setup CLI (mini_unzip + BCrypt + SwitchBaseRootSlot)
            r_upd = self.run_setup(exe, [
                "--cli", "apply-local-update", "--lang", "en",
                "--manifest", to_wine_path(mpath),
                "--archive", to_wine_path(zip6),
            ], wp)
            self.assertEqual(r_upd.returncode, 0, r_upd.stdout + r_upd.stderr)
            self.assertIn("CLI_OK:", r_upd.stdout)
            self.assertEqual((win64 / "dxgi.dll").read_bytes(), b"WIN_HOST_V106")
            self.assertEqual((win64 / "ReShade.ini").read_bytes(), orig_stub)
            self.assertEqual((base / "reshade-addons/RenoDX.addon64").read_bytes(), b"USER_RENODX_WIN")
            rs_after = ext_rs.read_text(encoding="utf-8")
            self.assertIn("PresetPath=UserPreset.ini", rs_after)
            self.assertIn("AddonPath=versions\\v1.0.6-22222222\\reshade-addons,reshade-addons", rs_after)

            led = json.loads((base / "runtime/install-ledger.json").read_text(encoding="utf-8"))
            self.assertEqual(led["installed_version"], "v1.0.6")
            self.assertEqual(led["active_slot"], "versions/v1.0.6-22222222")
            self.assertEqual(led["previous_slot"], "")
            self.assertEqual(led["previous_version"], "v1.0.5")

            # 3b. Drop an unverified rogue addon inside the managed slot and re-apply same v1.0.6:
            # MUST reject slot reuse, replace with clean slot, AND preserve previous_slot="" (v1.0.5)!
            rogue = base / "versions/v1.0.6-22222222/reshade-addons/rogue.addon64"
            rogue.write_bytes(b"ROGUE_WIN")
            r_idem = self.run_setup(exe, [
                "--cli", "apply-local-update", "--lang", "en",
                "--manifest", to_wine_path(mpath),
                "--archive", to_wine_path(zip6),
            ], wp)
            self.assertEqual(r_idem.returncode, 0, r_idem.stdout + r_idem.stderr)
            self.assertFalse(rogue.exists())
            led_idem = json.loads((base / "runtime/install-ledger.json").read_text(encoding="utf-8"))
            self.assertEqual(led_idem["previous_slot"], "")
            self.assertEqual(led_idem["previous_version"], "v1.0.5")

            # 4. Tampered archive MUST be rejected and keep v1.0.6 intact
            bad_zip = case / "tampered.zip"
            bad_bytes = bytearray(zip6.read_bytes())
            bad_bytes[-1] ^= 0xFF
            bad_zip.write_bytes(bytes(bad_bytes))
            r_bad = self.run_setup(exe, [
                "--cli", "apply-local-update", "--lang", "en",
                "--manifest", to_wine_path(mpath),
                "--archive", to_wine_path(bad_zip),
            ], wp)
            self.assertNotEqual(r_bad.returncode, 0)
            self.assertEqual((win64 / "dxgi.dll").read_bytes(), b"WIN_HOST_V106")

            # 5. Toggle auto-check preference off -> on
            r_off = self.run_setup(exe, ["--cli", "auto-check", "--mode", "off"], wp)
            self.assertEqual(r_off.returncode, 0, r_off.stdout + r_off.stderr)
            self.assertIn("auto_check=off", r_off.stdout)
            r_on = self.run_setup(exe, ["--cli", "auto-check", "--mode", "on"], wp)
            self.assertEqual(r_on.returncode, 0, r_on.stdout + r_on.stderr)
            self.assertIn("auto_check=on", r_on.stdout)

            # 6. One-click slot rollback v1.0.6 -> v1.0.5
            r_rb = self.run_setup(exe, ["--cli", "rollback", "--lang", "en"], wp)
            self.assertEqual(r_rb.returncode, 0, r_rb.stdout + r_rb.stderr)
            self.assertIn("CLI_OK:", r_rb.stdout)
            self.assertEqual((win64 / "dxgi.dll").read_bytes(), b"WIN_HOST_V105")
            self.assertEqual((base / "reshade-addons/RenoDX.addon64").read_bytes(), b"USER_RENODX_WIN")
            rs_rb = ext_rs.read_text(encoding="utf-8")
            self.assertIn("AddonPath=reshade-addons", rs_rb)
            self.assertNotIn("versions\\", rs_rb)
            self.assertIn("PresetPath=UserPreset.ini", rs_rb)


if __name__ == "__main__":
    unittest.main()
