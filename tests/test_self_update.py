#!/usr/bin/env python3
"""Unit tests for verified manifest-driven in-app slot self-update, rollback, and redirect/archive security."""
from __future__ import annotations

import hashlib
import importlib.util
import io
import json
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest import mock

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "scripts"))

import installer_core as core  # noqa: E402


def load_package_release():
    spec = importlib.util.spec_from_file_location("package_release_su_test", ROOT / "scripts/package_release.py")
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def populate_release_dir(target: Path, version: str, commit: str, host_tag: bytes, include_linux: bool = True) -> Path:
    target.mkdir(parents=True, exist_ok=True)
    for rel in core.REQUIRED_PAYLOAD:
        p = target / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        if rel == "host/ReShade64-game-forward.dll":
            p.write_bytes(host_tag)
        else:
            p.write_bytes(f"{version}:{rel}".encode("utf-8"))
    if include_linux:
        so = target / "lib/wine-filename-utf8.so"
        so.parent.mkdir(parents=True, exist_ok=True)
        so.write_bytes(b"\x7fELF" + host_tag)
        (target / "wwmi-dx12-setup").write_text(f"#!/bin/sh\n# {version}\n", encoding="utf-8")
        (target / "wwmi-dx12-setup").chmod(0o755)
    (target / "seed").mkdir(parents=True, exist_ok=True)
    (target / "seed/wwmi_dx12_seed.texlook").write_bytes(b"SEED-" + version.encode("utf-8"))
    (target / "BUILD-PROVENANCE.json").write_text(
        json.dumps({"version": version, "repository_commit": commit, "working_tree_dirty": False}, indent=2) + "\n",
        encoding="utf-8",
    )
    return target


class SelfUpdateTests(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory(prefix="wwmi-self-update-test-")
        self.case = Path(self._tmp.name)
        self.win64 = self.case / "Game/Wuthering Waves Game/Client/Binaries/Win64"
        self.win64.mkdir(parents=True)
        (self.win64 / core.TARGET_SHIPPING_EXE).write_bytes(b"MZ_FAKE_GAME")
        self.wwmi = self.case / "WWMI"
        core.init_wwmi_skeleton(self.wwmi)
        (self.wwmi / "Mods/SampleSkin").mkdir(parents=True)
        (self.wwmi / "Mods/SampleSkin/mod.ini").write_text("; sample\n", encoding="utf-8")
        self.staging = self.case / "staging"
        self.base_root = self.case / "WWMI-DX12-RT"

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def test_redirect_url_validation_rejects_evil_hosts_and_http(self) -> None:
        # Allowed initial hop
        core.validate_update_hop_url(core.UPDATE_MANIFEST_URL, is_initial_hop=True)
        core.validate_update_hop_url(
            "https://github.com/Bud-Open/wwmi-dx12-rt/releases/download/v1.0.6/WWMI-DX12-RT-linux.tar.gz",
            is_initial_hop=True,
        )
        # Allowed CDN redirect hops
        core.validate_update_hop_url("https://release-assets.githubusercontent.com/some/asset", is_initial_hop=False)
        core.validate_update_hop_url("https://objects.githubusercontent.com/some/asset", is_initial_hop=False)

        for bad_init in (
            "http://github.com/Bud-Open/wwmi-dx12-rt/releases/latest/download/update-manifest.json",
            "https://github.com/OtherOrg/wwmi-dx12-rt/releases/latest/download/update-manifest.json",
            "https://evil.example.com/update-manifest.json",
            "https://user:pass@github.com/Bud-Open/wwmi-dx12-rt/releases/latest/download/update-manifest.json",
            "https://github.com:8443/Bud-Open/wwmi-dx12-rt/releases/latest/download/update-manifest.json",
        ):
            with self.assertRaises(RuntimeError, msg=f"Should reject initial URL {bad_init}"):
                core.validate_update_hop_url(bad_init, is_initial_hop=True)

        for bad_redir in (
            "http://objects.githubusercontent.com/foo",
            "https://evil.githubusercontent.com/foo",
            "https://github.com/OtherOrg/repo/releases/download/v1/x.zip",
        ):
            with self.assertRaises(RuntimeError, msg=f"Should reject redirect URL {bad_redir}"):
                core.validate_update_hop_url(bad_redir, is_initial_hop=False)

    def test_end_to_end_slot_update_preserves_user_data_and_rolls_back_cleanly(self) -> None:
        pkg = load_package_release()
        src_v5 = populate_release_dir(self.case / "src-v1.0.5", "v1.0.5", "11111111" * 5, b"HOST_V105_BYTES")
        # 1. Initial install v1.0.5 into fixed base_root
        core.install_mount(
            source_root=src_v5,
            win64_dir=self.win64,
            wwmi_dir=self.wwmi,
            base_root=self.base_root,
            platform_kind="linux-proton",
            staging_root=self.staging,
            check_processes=False,
        )
        # User adds third-party add-on + custom ReShade preset + custom wwmi_dx12.ini comment
        (self.base_root / "reshade-addons/RenoDX.addon64").write_bytes(b"USER_RENODX")
        ext_rs = self.base_root / "ReShade.ini"
        ext_rs.write_text(ext_rs.read_text(encoding="utf-8") + "\n[GENERAL]\nPresetPath=MyCustom.ini\n", encoding="utf-8")
        orig_wwmi_ini = (self.base_root / "wwmi_dx12.ini").read_bytes()
        orig_stub = (self.win64 / "ReShade.ini").read_bytes()

        # 2. Build release v1.0.6 tar.gz + update-manifest.json
        rel_v6_dir = populate_release_dir(self.case / "WWMI-DX12-RT-linux", "v1.0.6", "22222222" * 5, b"HOST_V106_BYTES")
        tar_v6 = self.case / "WWMI-DX12-RT-linux.tar.gz"
        pkg.make_targz(rel_v6_dir, tar_v6)
        manifest_v6 = pkg.build_update_manifest(
            {"version": "v1.0.6", "repository_commit": "22222222" * 5},
            {"linux-x64": pkg.build_asset_manifest_entry(rel_v6_dir, tar_v6, "tar.gz", {"wwmi-dx12-setup"})},
        )
        manifest_bytes = json.dumps(manifest_v6).encode("utf-8")
        tar_bytes = tar_v6.read_bytes()

        def fake_urlopen(req, timeout=8.0):
            url = req.full_url
            if url == core.UPDATE_MANIFEST_URL:
                return io.BytesIO(manifest_bytes)
            if url.endswith("/v1.0.6/WWMI-DX12-RT-linux.tar.gz"):
                return io.BytesIO(tar_bytes)
            raise AssertionError(f"Unexpected URL requested: {url}")

        # 3. Execute seamless in-app self-update v1.0.5 -> v1.0.6
        res = core.perform_self_update(
            self.base_root,
            platform_kind="linux-proton",
            staging_root=self.staging,
            check_processes=False,
            urlopen_fn=fake_urlopen,
        )
        self.assertEqual(res["state"], "updated")
        self.assertEqual(res["installed_version"], "v1.0.6")
        self.assertTrue(res["active_slot"].startswith("versions/v1.0.6-"))
        self.assertEqual((self.win64 / "dxgi.dll").read_bytes(), b"HOST_V106_BYTES")
        self.assertEqual((self.win64 / "ReShade.ini").read_bytes(), orig_stub)  # BasePath unchanged!
        self.assertEqual((self.base_root / "lib/wine-filename-utf8.so").read_bytes(), b"\x7fELFHOST_V106_BYTES")
        # User third-party addon, wwmi_dx12.ini, and ReShade custom preset preserved!
        self.assertEqual((self.base_root / "reshade-addons/RenoDX.addon64").read_bytes(), b"USER_RENODX")
        self.assertEqual((self.base_root / "wwmi_dx12.ini").read_bytes(), orig_wwmi_ini)
        new_rs_text = ext_rs.read_text(encoding="utf-8")
        self.assertIn("PresetPath=MyCustom.ini", new_rs_text)
        self.assertIn(f"AddonPath={res['active_slot'].replace('/', chr(92))}\\reshade-addons,reshade-addons", new_rs_text)

        # 4. Mid-swap crash recovery: simulate failure when swapping to v1.0.7 -> must restore v1.0.6 cleanly!
        rel_v7_dir = populate_release_dir(self.case / "v7_stage/WWMI-DX12-RT-linux", "v1.0.7", "33333333" * 5, b"HOST_V107_BYTES")
        tar_v7 = self.case / "WWMI-DX12-RT-linux-v7.tar.gz"
        pkg.make_targz(rel_v7_dir, tar_v7)
        entry_v7 = pkg.build_asset_manifest_entry(rel_v7_dir, tar_v7, "tar.gz", {"wwmi-dx12-setup"})
        entry_v7["name"] = "WWMI-DX12-RT-linux.tar.gz"
        manifest_v7 = pkg.build_update_manifest(
            {"version": "v1.0.7", "repository_commit": "33333333" * 5},
            {"linux-x64": entry_v7},
        )
        info_v7 = core.parse_update_manifest(manifest_v7, "v1.0.6", "linux-proton")
        slot_v7, _ = core.extract_verified_update_slot(tar_v7, info_v7, self.base_root)
        with mock.patch.object(core, "_write_new_file", side_effect=OSError("simulated disk full")):
            with self.assertRaises(OSError):
                core.apply_slot_update(self.base_root, slot_v7, info_v7, staging_root=self.staging, check_processes=False)
        # v1.0.6 must remain 100% intact!
        self.assertEqual((self.win64 / "dxgi.dll").read_bytes(), b"HOST_V106_BYTES")
        self.assertEqual(core.load_install_ledger(self.base_root)["installed_version"], "v1.0.6")
        self.assertIn(res["active_slot"].replace("/", "\\"), ext_rs.read_text(encoding="utf-8"))

        # 5. One-click slot rollback v1.0.6 -> v1.0.5 (flat initial root)
        rb = core.rollback_mount(self.base_root, staging_root=self.staging, check_processes=False)
        self.assertEqual(rb["state"], "rolled_back")
        self.assertEqual(rb["target_version"], "v1.0.5")
        self.assertEqual((self.win64 / "dxgi.dll").read_bytes(), b"HOST_V105_BYTES")
        self.assertEqual((self.base_root / "reshade-addons/RenoDX.addon64").read_bytes(), b"USER_RENODX")
        self.assertIn("AddonPath=reshade-addons", ext_rs.read_text(encoding="utf-8"))
        self.assertNotIn("versions\\", ext_rs.read_text(encoding="utf-8"))
        self.assertIn("PresetPath=MyCustom.ini", ext_rs.read_text(encoding="utf-8"))

    def test_tampered_archive_and_tar_slip_are_rejected(self) -> None:
        pkg = load_package_release()
        rel_dir = populate_release_dir(self.case / "t_src/WWMI-DX12-RT-linux", "v1.0.6", "44444444" * 5, b"HOST_V106")
        tar_ok = self.case / "ok.tar.gz"
        pkg.make_targz(rel_dir, tar_ok)
        entry = pkg.build_asset_manifest_entry(rel_dir, tar_ok, "tar.gz", {"wwmi-dx12-setup"})
        entry["name"] = "WWMI-DX12-RT-linux.tar.gz"
        manifest = pkg.build_update_manifest({"version": "v1.0.6", "repository_commit": "44444444" * 5}, {"linux-x64": entry})
        info = core.parse_update_manifest(manifest, "v1.0.5", "linux-proton")

        # Tamper 1 byte in archive -> download_verified_asset MUST abort
        bad_bytes = bytearray(tar_ok.read_bytes())
        bad_bytes[-1] ^= 0xFF
        with self.assertRaises(RuntimeError):
            core.download_verified_asset(
                info,
                self.case / "dl.tar.gz",
                urlopen_fn=lambda req, timeout=8.0: io.BytesIO(bytes(bad_bytes)),
            )

        # Tar-Slip symlink archive -> extract_verified_update_slot MUST abort
        evil_tar = self.case / "evil.tar.gz"
        with tarfile.open(evil_tar, "w:gz") as tf:
            ti = tarfile.TarInfo("WWMI-DX12-RT-linux/evil_link")
            ti.type = tarfile.SYMTYPE
            ti.linkname = "/etc/passwd"
            tf.addfile(ti)
        evil_info = dict(info)
        evil_info["asset_bytes"] = evil_tar.stat().st_size
        evil_info["asset_sha256"] = hashlib.sha256(evil_tar.read_bytes()).hexdigest()
        with self.assertRaises(RuntimeError):
            core.extract_verified_update_slot(evil_tar, evil_info, self.base_root)

    def test_auto_check_prefs_and_future_protocol_guard(self) -> None:
        self.assertTrue(core.load_setup_prefs(self.base_root)["auto_check_updates"])
        core.save_setup_prefs(self.base_root, False)
        self.assertFalse(core.load_setup_prefs(self.base_root)["auto_check_updates"])

        pkg = load_package_release()
        rel_dir = populate_release_dir(self.case / "p_src/WWMI-DX12-RT-linux", "v2.0.0", "55555555" * 5, b"HOST_V2")
        tar_p = self.case / "p.tar.gz"
        pkg.make_targz(rel_dir, tar_p)
        entry = pkg.build_asset_manifest_entry(rel_dir, tar_p, "tar.gz", {"wwmi-dx12-setup"})
        entry["name"] = "WWMI-DX12-RT-linux.tar.gz"
        manifest = pkg.build_update_manifest({"version": "v2.0.0", "repository_commit": "55555555" * 5}, {"linux-x64": entry})
        manifest["min_updater_protocol"] = 99
        info = core.parse_update_manifest(manifest, "v1.0.5", "linux-proton")
        self.assertTrue(info["requires_manual_upgrade"])
        with self.assertRaises(RuntimeError):
            core.perform_self_update(self.base_root, platform_kind="linux-proton", manifest_info=info)

    def test_p1_hardening_extra_slot_file_repair_shim_rollback_and_preinstall_stage(self) -> None:
        pkg = load_package_release()

        # Case A: Pre-install self-update stages v1.0.6, then initial Install activates v1.0.6 directly!
        base_pre = self.case / "pre_base"
        win64_pre = self.case / "pre_game/Wuthering Waves Game/Client/Binaries/Win64"
        wwmi_pre = self.case / "pre_wwmi"
        base_pre.mkdir(parents=True)
        win64_pre.mkdir(parents=True)
        (win64_pre / "Client-Win64-Shipping.exe").write_bytes(b"GAME")
        wwmi_pre.mkdir(parents=True)
        (wwmi_pre / "d3dx.ini").write_text("[Loader]\n", encoding="utf-8")
        src_v5 = populate_release_dir(self.case / "p1_v105", "v1.0.5", "11111111" * 5, b"HOST_V105")
        (src_v5 / "lib/wine-filename-utf8.so").write_bytes(b"SHIM_V105")

        rel_v6 = populate_release_dir(self.case / "p1_v106/WWMI-DX12-RT-linux", "v1.0.6", "22222222" * 5, b"HOST_V106")
        (rel_v6 / "lib/wine-filename-utf8.so").write_bytes(b"SHIM_V106")
        tar_v6 = self.case / "p1_v106.tar.gz"
        pkg.make_targz(rel_v6, tar_v6)
        entry_v6 = pkg.build_asset_manifest_entry(rel_v6, tar_v6, "tar.gz", {"wwmi-dx12-setup"})
        entry_v6["name"] = "WWMI-DX12-RT-linux.tar.gz"
        manifest_v6 = pkg.build_update_manifest({"version": "v1.0.6", "repository_commit": "22222222" * 5}, {"linux-x64": entry_v6})
        info_v6 = core.parse_update_manifest(manifest_v6, "v1.0.5", "linux-proton")

        st_res = core.perform_self_update(
            base_pre,
            current_version="v1.0.5",
            platform_kind="linux-proton",
            staging_root=self.staging,
            check_processes=False,
            urlopen_fn=lambda req, timeout=8.0: io.BytesIO(tar_v6.read_bytes()),
            manifest_info=info_v6,
        )
        self.assertEqual(st_res["state"], "staged_not_installed")
        self.assertIsNotNone(core.load_staged_release(base_pre))

        # Now install from v1.0.5 source -> MUST honor staged v1.0.6 slot automatically!
        inst_res = core.install_mount(
            source_root=src_v5,
            win64_dir=win64_pre,
            wwmi_dir=wwmi_pre,
            base_root=base_pre,
            platform_kind="linux-proton",
            staging_root=self.staging,
            check_processes=False,
        )
        self.assertEqual(inst_res["installed_version"], "v1.0.6")
        self.assertEqual(inst_res["active_slot"], st_res["active_slot"])
        self.assertEqual((win64_pre / "dxgi.dll").read_bytes(), b"HOST_V106")
        self.assertIn(st_res["active_slot"].replace("/", "\\"), (base_pre / "ReShade.ini").read_text(encoding="utf-8"))
        self.assertIsNone(core.load_staged_release(base_pre))

        # Case B: Installed v1.0.5 -> self-update v1.0.6 -> verify extra untrusted file invalidates slot reuse,
        # idempotent re-update preserves rollback target, Repair preserves active_slot, and rollback restores flat shim!
        core.install_mount(
            source_root=src_v5,
            win64_dir=self.win64,
            wwmi_dir=self.wwmi,
            base_root=self.base_root,
            platform_kind="linux-proton",
            staging_root=self.staging,
            check_processes=False,
        )
        self.assertEqual((self.base_root / "lib/wine-filename-utf8.so").read_bytes(), b"SHIM_V105")

        up1 = core.perform_self_update(
            self.base_root,
            platform_kind="linux-proton",
            staging_root=self.staging,
            check_processes=False,
            urlopen_fn=lambda req, timeout=8.0: io.BytesIO(tar_v6.read_bytes()),
            manifest_info=info_v6,
        )
        self.assertEqual(up1["state"], "updated")
        self.assertEqual((self.base_root / "lib/wine-filename-utf8.so").read_bytes(), b"SHIM_V106")
        slot_v6_dir = Path(up1["to_root"])

        # Drop an unverified extra .addon64 inside the managed slot -> MUST fail strict verification!
        rogue = slot_v6_dir / "reshade-addons/rogue_unverified.addon64"
        rogue.write_bytes(b"ROGUE")
        self.assertFalse(core.verify_slot_against_manifest(slot_v6_dir, info_v6))

        # Re-extracting/applying same v1.0.6 replaces polluted slot AND preserves previous_slot="" (v1.0.5)!
        up_idem = core.apply_slot_update(
            self.base_root,
            up1["active_slot"],
            info_v6,
            staging_root=self.staging,
            check_processes=False,
            staged_slot_dir=core._extract_archive_to_staging_dir(tar_v6, info_v6, self.base_root)[1],
        )
        self.assertFalse(rogue.exists())
        led_idem = core.load_install_ledger(self.base_root)
        self.assertEqual(led_idem["previous_slot"], "")
        self.assertEqual(led_idem["previous_version"], "v1.0.5")

        # In-place Reapply/Repair MUST keep active_slot=v1.0.6 AND previous_slot=""!
        rep = core.install_mount(
            source_root=src_v5,
            win64_dir=self.win64,
            wwmi_dir=self.wwmi,
            base_root=self.base_root,
            platform_kind="linux-proton",
            staging_root=self.staging,
            check_processes=False,
        )
        self.assertEqual(rep["installed_version"], "v1.0.6")
        self.assertEqual(rep["active_slot"], up1["active_slot"])
        self.assertEqual(rep["previous_slot"], "")
        self.assertEqual((self.win64 / "dxgi.dll").read_bytes(), b"HOST_V106")

        # Roll back v1.0.6 -> flat v1.0.5 MUST restore the original flat lib/wine-filename-utf8.so!
        rb = core.rollback_mount(self.base_root, staging_root=self.staging, check_processes=False)
        self.assertEqual(rb["target_version"], "v1.0.5")
        self.assertEqual((self.win64 / "dxgi.dll").read_bytes(), b"HOST_V105")
        self.assertEqual((self.base_root / "lib/wine-filename-utf8.so").read_bytes(), b"SHIM_V105")


if __name__ == "__main__":
    unittest.main()
