import configparser
import importlib.util
import io
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "tools"))

import installer_core as core  # noqa: E402


def load_script(name: str, relative: str):
    spec = importlib.util.spec_from_file_location(name, REPO_ROOT / relative)
    assert spec and spec.loader
    module = importlib.util.module_from_spec(spec)
    with patch.object(sys, "path", [str(Path(spec.origin).parent), *sys.path]):
        spec.loader.exec_module(module)
    return module


def make_game(root: Path) -> Path:
    win64 = root / "Client/Binaries/Win64"
    win64.mkdir(parents=True)
    (win64 / core.TARGET_SHIPPING_EXE).touch()
    return win64


def populate_source(src: Path, version: str | None = None, commit: str | None = None, host_bytes: bytes = b"HOST_DLL") -> Path:
    for name in (*core.REQUIRED_PAYLOAD, "lib/wine-filename-utf8.so"):
        target = src / name
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(host_bytes if name == "host/ReShade64-game-forward.dll" else name.encode("ascii"))
    if version is not None or commit is not None:
        prov = {}
        if version is not None:
            prov["version"] = version
        if commit is not None:
            prov["repository_commit"] = commit
        (src / "BUILD-PROVENANCE.json").write_text(json.dumps(prov, indent=2) + "\n", encoding="utf-8")
    return src


class TestInstallerUpgrade(unittest.TestCase):
    def setUp(self) -> None:
        self._lang_patch = patch.dict(os.environ, {"WWMI_SETUP_LANG": "zh"})
        self._lang_patch.start()
        self.addCleanup(self._lang_patch.stop)
        self.case = Path(tempfile.mkdtemp(prefix="wwmi-upgrade-test-")).resolve()
        self.win64 = make_game(self.case / "Game/Wuthering Waves Game")
        self.staging = self.case / "Staging"
        self.wwmi = core.init_wwmi_skeleton(self.case / "SharedWWMI")
        (self.wwmi / "Mods/SampleSkin").mkdir(parents=True)
        (self.wwmi / "Mods/SampleSkin/mod.ini").write_text("[TextureOverride]\n", encoding="utf-8")

        self.src_a = populate_source(self.case / "SrcA", version="v1.0.4", commit="1111111aaaaaaa", host_bytes=b"HOST_V104")
        self.src_b = populate_source(self.case / "SrcB", version="v1.0.5", commit="2222222bbbbbbb", host_bytes=b"HOST_V105")
        self.base_a = self.case / "InstallA"
        self.base_b = self.case / "InstallB"

    def test_provenance_version_resolution_and_sha256sums(self) -> None:
        pkg = load_script("package_release_m1", "scripts/package_release.py")

        self.assertEqual(pkg.resolve_release_version("v1.0.5", self.case, "abcdef123456"), "v1.0.5")
        self.assertEqual(pkg.resolve_release_version(None, self.case, "abcdef123456"), "dev-abcdef1")
        with patch.object(pkg, "git_output", side_effect=lambda _r, *args: "v1.0.5" if args[:2] == ("describe", "--tags") else "abcdef1"):
            fake_repo = self.case / "FakeRepo"
            (fake_repo / ".git").mkdir(parents=True)
            self.assertEqual(pkg.resolve_release_version(None, fake_repo, "abcdef123456"), "v1.0.5")

        prov = pkg.build_provenance({"repository_commit": "deadbeef1234", "working_tree_dirty": False}, version="v1.0.5")
        self.assertEqual(prov["version"], "v1.0.5")
        self.assertEqual(prov["repository_commit"], "deadbeef1234")
        self.assertFalse(prov["working_tree_dirty"])

        arch_win = self.case / "WWMI-DX12-RT-win64.zip"
        arch_lin = self.case / "WWMI-DX12-RT-linux.tar.gz"
        arch_win.write_bytes(b"win-archive-bytes")
        arch_lin.write_bytes(b"linux-archive-bytes")
        sums_file = pkg.write_sha256sums([arch_win, arch_lin], self.case / "SHA256SUMS.txt")
        lines = sums_file.read_text(encoding="utf-8").splitlines()
        self.assertEqual(lines, [
            f"{core.sha256_file(arch_win)}  WWMI-DX12-RT-win64.zip",
            f"{core.sha256_file(arch_lin)}  WWMI-DX12-RT-linux.tar.gz",
        ])

        # Local provenance reading + legacy ledger display without installed_version
        self.assertEqual(core.read_package_provenance(self.src_b)["version"], "v1.0.5")
        self.assertEqual(core.read_package_provenance(self.case / "NoProv")["version"], "dev")
        self.assertEqual(core.format_installed_version({"state": "installed"}), "旧版（版本未知）")
        with patch.dict(os.environ, {"WWMI_SETUP_LANG": "en"}):
            self.assertEqual(core.format_installed_version({"state": "installed"}), "Legacy (version unknown)")

        # Doctor shows legacy version cleanly for pre-v1.0.5 ledgers
        core.install_mount(self.src_a, self.win64, self.wwmi, self.base_a, staging_root=self.staging, check_processes=False)
        led_path = self.base_a / "runtime/install-ledger.json"
        led = json.loads(led_path.read_text(encoding="utf-8"))
        self.assertEqual(led["installed_version"], "v1.0.4")
        self.assertEqual(led["installed_commit"], "1111111aaaaaaa")
        led.pop("installed_version")
        led.pop("installed_commit")
        led_path.write_text(json.dumps(led), encoding="utf-8")
        rep = core.run_doctor(self.win64, self.wwmi, self.base_a, source_root=self.src_a)
        self.assertEqual(rep.installed_version, "旧版（版本未知）")

    def test_canonical_win64_lock_contention_across_base_roots(self) -> None:
        core.install_mount(self.src_a, self.win64, self.wwmi, self.base_a, staging_root=self.staging, check_processes=False)
        # Holding canonical Win64 lock blocks a concurrent upgrade from a completely different base_root B.
        with core._win64_lock(self.win64):
            with self.assertRaisesRegex(RuntimeError, "无法锁定目标 Win64"):
                core.install_mount(self.src_b, self.win64, self.wwmi, self.base_b, staging_root=self.staging, check_processes=False)

    def test_verified_side_by_side_upgrade_and_rollback(self) -> None:
        res_a = core.install_mount(
            self.src_a, self.win64, self.wwmi, self.base_a,
            platform_kind="linux-proton", staging_root=self.staging, check_processes=False,
        )
        self.assertEqual(res_a["state"], "installed")
        self.assertEqual((self.win64 / "dxgi.dll").read_bytes(), b"HOST_V104")

        # B detects verified upgrade_ready from A (v1.0.4 -> v1.0.5)
        action = core.describe_install_action(self.win64, self.base_b, source_root=self.src_b, platform_kind="linux-proton")
        self.assertEqual(action["action"], "upgrade")
        self.assertEqual(action["mount_state"], "upgrade_ready")
        self.assertEqual(action["source_version"], "v1.0.4")
        self.assertEqual(action["target_version"], "v1.0.5")
        self.assertEqual(action["owner_base_root"], str(self.base_a))

        # Side-by-side upgrade A -> B
        res_b = core.install_mount(
            self.src_b, self.win64, self.base_b / "WWMI", self.base_b,
            platform_kind="linux-proton", staging_root=self.staging, check_processes=False, init_missing_wwmi=True,
        )
        self.assertEqual(res_b["state"], "installed")
        self.assertEqual(res_b["install_mode"], "upgrade")
        self.assertEqual(res_b["installed_version"], "v1.0.5")
        self.assertEqual(res_b["installed_commit"], "2222222bbbbbbb")
        self.assertEqual(res_b["upgraded_from"], str(self.base_a))
        # Populated WWMI dir from A was retained instead of creating an empty B/WWMI skeleton
        self.assertEqual(res_b["wwmi_dir"], str(self.wwmi))
        self.assertTrue(res_b["retained_wwmi_dir"])
        self.assertFalse((self.base_b / "WWMI").exists())
        self.assertEqual((self.win64 / "dxgi.dll").read_bytes(), b"HOST_V105")
        self.assertIn(core.format_engine_path(self.base_b, "linux-proton"), (self.win64 / "ReShade.ini").read_text(encoding="utf-8"))
        # Linux base_root change emits LD_PRELOAD reminder
        self.assertTrue(any("LD_PRELOAD" in w for w in res_b["migration_warnings"]))

        # A is preserved (NEVER deleted!) and its ledger records migrated_to: B
        self.assertTrue(self.base_a.is_dir())
        self.assertTrue((self.base_a / "host/ReShade64-game-forward.dll").is_file())
        led_a = json.loads((self.base_a / "runtime/install-ledger.json").read_text(encoding="utf-8"))
        self.assertEqual(led_a["state"], "uninstalled")
        self.assertEqual(led_a["migrated_to"], str(self.base_b))

        # Rollback B -> A via the exact same verified takeover path
        rb_action = core.describe_install_action(self.win64, self.base_a, source_root=self.src_a, platform_kind="linux-proton")
        self.assertEqual(rb_action["action"], "rollback")
        res_rb = core.install_mount(
            self.src_a, self.win64, self.wwmi, self.base_a,
            platform_kind="linux-proton", staging_root=self.staging, check_processes=False,
        )
        self.assertEqual(res_rb["state"], "installed")
        self.assertEqual(res_rb["install_mode"], "rollback")
        self.assertEqual(res_rb["upgraded_from"], str(self.base_b))
        self.assertEqual((self.win64 / "dxgi.dll").read_bytes(), b"HOST_V104")
        led_b_after = json.loads((self.base_b / "runtime/install-ledger.json").read_text(encoding="utf-8"))
        self.assertEqual(led_b_after["state"], "uninstalled")
        self.assertEqual(led_b_after["migrated_to"], str(self.base_a))

    def test_rejects_foreign_tampered_installing_and_missing_win64_states(self) -> None:
        # 1. Foreign dxgi.dll + ReShade.ini without ledger
        (self.win64 / "dxgi.dll").write_bytes(b"FOREIGN")
        (self.win64 / "ReShade.ini").write_text("[INSTALL]\nBasePath=Z:\\nonexistent\\dir\n", encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "不存在"):
            core.install_mount(self.src_b, self.win64, self.wwmi, self.base_b, platform_kind="linux-proton",
                               staging_root=self.staging, check_processes=False)
        self.assertFalse(self.base_b.exists())
        core.stage_files_for_removal([self.win64 / "dxgi.dll", self.win64 / "ReShade.ini"], self.staging, "clean")

        # Install clean A
        core.install_mount(self.src_a, self.win64, self.wwmi, self.base_a, platform_kind="linux-proton",
                           staging_root=self.staging, check_processes=False)

        # 2. Tampered dxgi.dll -> rejected
        orig_dxgi = (self.win64 / "dxgi.dll").read_bytes()
        (self.win64 / "dxgi.dll").write_bytes(b"TAMPERED")
        with self.assertRaisesRegex(RuntimeError, "哈希"):
            core.install_mount(self.src_b, self.win64, self.wwmi, self.base_b, platform_kind="linux-proton",
                               staging_root=self.staging, check_processes=False)
        (self.win64 / "dxgi.dll").write_bytes(orig_dxgi)

        # 3. Missing dxgi.dll -> rejected (instructs repair/uninstall in old folder)
        batch = core.stage_files_for_removal([self.win64 / "dxgi.dll"], self.staging, "drop-dxgi")
        with self.assertRaisesRegex(RuntimeError, "不完整"):
            core.install_mount(self.src_b, self.win64, self.wwmi, self.base_b, platform_kind="linux-proton",
                               staging_root=self.staging, check_processes=False)
        (self.win64 / "dxgi.dll").write_bytes((batch / "000_dxgi.dll").read_bytes())

        # 4. Old ledger in 'installing' state -> rejected
        led_a_path = self.base_a / "runtime/install-ledger.json"
        orig_led_a = led_a_path.read_text(encoding="utf-8")
        led_a = json.loads(orig_led_a)
        led_a["state"] = "installing"
        led_a["pending_id"] = "a" * 32
        led_a_path.write_text(json.dumps(led_a), encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "installing"):
            core.install_mount(self.src_b, self.win64, self.wwmi, self.base_b, platform_kind="linux-proton",
                               staging_root=self.staging, check_processes=False)
        led_a_path.write_text(orig_led_a, encoding="utf-8")

    def test_mixed_state_auto_rollback_and_crash_recovery(self) -> None:
        core.install_mount(self.src_a, self.win64, self.wwmi, self.base_a, platform_kind="linux-proton",
                           staging_root=self.staging, check_processes=False)
        orig_dxgi = (self.win64 / "dxgi.dll").read_bytes()
        orig_ini = (self.win64 / "ReShade.ini").read_bytes()

        # Part A: live step-2 failure on ReShade.ini -> automatic rollback of dxgi.dll to clean A!
        real_write = core._write_new_file

        def fail_second_swap(path: Path, data: bytes) -> None:
            if path.name.endswith("-ReShade.ini.tmp"):
                # Confirm dxgi.dll was already replaced before ReShade.ini fails
                self.assertEqual((self.win64 / "dxgi.dll").read_bytes(), b"HOST_V105")
                real_write(path, data[:4])
                raise OSError("injected second-file swap failure")
            return real_write(path, data)

        with patch.object(core, "_write_new_file", side_effect=fail_second_swap):
            with self.assertRaisesRegex(OSError, "injected second-file swap failure"):
                core.install_mount(self.src_b, self.win64, self.wwmi, self.base_b, platform_kind="linux-proton",
                                   staging_root=self.staging, check_processes=False)

        # Win64 must be 100% restored to A's exact bytes and no leftover .tmp in Win64!
        self.assertEqual((self.win64 / "dxgi.dll").read_bytes(), orig_dxgi)
        self.assertEqual((self.win64 / "ReShade.ini").read_bytes(), orig_ini)
        self.assertCountEqual(os.listdir(self.win64), [core.TARGET_SHIPPING_EXE, *core.BOOTSTRAP_FILES])
        self.assertEqual(core.inspect_mount_state(self.win64, self.base_a, platform_kind="linux-proton")[0], "installed")

        # Part B: simulate hard kill (SIGKILL) leaving mixed state (dxgi.dll=B, ReShade.ini=A) + installing ledger in B
        rec_dir = self.base_b / "runtime/recovery" / ("f" * 32)
        rec_dir.mkdir(parents=True, exist_ok=True)
        bkp_dxgi = rec_dir / "dxgi.dll"
        bkp_ini = rec_dir / "ReShade.ini"
        bkp_dxgi.write_bytes(orig_dxgi)
        bkp_ini.write_bytes(orig_ini)
        new_ini = core.build_win64_stub_reshade_ini(core.format_engine_path(self.base_b, "linux-proton")).encode("utf-8")
        (self.win64 / "dxgi.dll").write_bytes(b"HOST_V105")  # new B
        # ReShade.ini still old A!
        mixed_ledger = {
            "schema_version": 1, "state": "installing", "pending_id": "f" * 32,
            "platform_kind": "linux-proton", "base_root": str(self.base_b),
            "win64_dir": str(self.win64), "wwmi_dir": str(self.wwmi),
            "previous_owner": str(self.base_a), "recovery_dir": str(rec_dir),
            "win64_files": [
                {"relative": "dxgi.dll", "path": str(self.win64 / "dxgi.dll"),
                 "sha256": core.sha256_file(self.src_b / "host/ReShade64-game-forward.dll"),
                 "previous_sha256": core.sha256_file(bkp_dxgi), "recovery_backup": str(bkp_dxgi), "size": 9},
                {"relative": "ReShade.ini", "path": str(self.win64 / "ReShade.ini"),
                 "sha256": core.hashlib.sha256(new_ini).hexdigest(),
                 "previous_sha256": core.sha256_file(bkp_ini), "recovery_backup": str(bkp_ini), "size": len(new_ini)},
            ],
            "extra_managed_files": [],
        }
        (self.base_b / "runtime/install-ledger.json").write_text(json.dumps(mixed_ledger), encoding="utf-8")
        self.assertEqual(core.inspect_mount_state(self.win64, self.base_b, platform_kind="linux-proton")[0], "installing")

        # Retrying B recovers from mixed state cleanly and completes migration from A -> B!
        recovered = core.install_mount(self.src_b, self.win64, self.wwmi, self.base_b, platform_kind="linux-proton",
                                       staging_root=self.staging, check_processes=False)
        self.assertEqual(recovered["state"], "installed")
        self.assertEqual(recovered["upgraded_from"], str(self.base_a))
        self.assertEqual((self.win64 / "dxgi.dll").read_bytes(), b"HOST_V105")
        self.assertEqual((self.win64 / "ReShade.ini").read_bytes(), new_ini)
        led_a_after = json.loads((self.base_a / "runtime/install-ledger.json").read_text(encoding="utf-8"))
        self.assertEqual(led_a_after["migrated_to"], str(self.base_b))

        # Part C: aborting an interrupted mixed-state upgrade via uninstall restores A cleanly!
        # Re-establish A -> mixed B
        core.install_mount(self.src_a, self.win64, self.wwmi, self.base_a, platform_kind="linux-proton",
                           staging_root=self.staging, check_processes=False)
        (self.win64 / "dxgi.dll").write_bytes(b"HOST_V105")
        (self.base_b / "runtime/install-ledger.json").write_text(json.dumps(mixed_ledger), encoding="utf-8")
        un = core.uninstall_mount(self.base_b, staging_root=self.staging, check_processes=False)
        self.assertEqual(un["restored_previous_owner"], str(self.base_a))
        self.assertEqual((self.win64 / "dxgi.dll").read_bytes(), orig_dxgi)
        self.assertEqual((self.win64 / "ReShade.ini").read_bytes(), orig_ini)

    def test_user_data_migration_ini_utf16le_reshade_cache_and_addons(self) -> None:
        core.install_mount(self.src_a, self.win64, self.wwmi, self.base_a, platform_kind="linux-proton",
                           staging_root=self.staging, check_processes=False)

        # Customize A's ReShade.ini with LoadGameDirAddons, absolute old path, and relative preset path
        old_eng = core.format_engine_path(self.base_a, "linux-proton")
        (self.base_a / "ReShade.ini").write_text(
            "[INSTALL]\nLogging=1\n\n"
            "[ADDON]\nAddonPath=reshade-addons\nLoadGameDirAddons=1\n"
            f"ExtraAddonPaths={old_eng}\\custom-addons\n\n"
            "[GENERAL]\nPresetPath=.\\Presets\\MyLook.ini\n",
            encoding="utf-8",
        )
        # Customize A's UTF-16LE wwmi_dx12.ini with custom user settings
        ini_a = (self.base_a / "wwmi_dx12.ini").read_bytes()[2:].decode("utf-16-le")
        ini_a = ini_a.replace("debug=0", "debug=1").replace("skin_mode=base", "skin_mode=exact") + "custom_flag=42\r\n"
        (self.base_a / "wwmi_dx12.ini").write_bytes(b"\xff\xfe" + ini_a.encode("utf-16-le"))

        # Populate A's cache + newer pre-existing cache in B
        cache_old = self.base_a / "runtime/cache/mod_tex_adapt.cache"
        cache_old.write_bytes(b"ADAPT_CACHE_A")
        cache_skip = self.base_a / "runtime/cache/lod_tex_cache.lodcache"
        cache_skip.write_bytes(b"OLD_LOD")
        os.utime(cache_skip, (1000, 1000))
        (self.base_b / "runtime/cache").mkdir(parents=True, exist_ok=True)
        b_newer = self.base_b / "runtime/cache/lod_tex_cache.lodcache"
        b_newer.write_bytes(b"NEWER_LOD_IN_B")
        os.utime(b_newer, (2000, 2000))

        # Populate third-party addons in A and a conflicting one in B
        (self.base_a / "reshade-addons/renodx-ww.addon64").write_bytes(b"RENODX_A")
        (self.base_a / "reshade-addons/conflict.addon64").write_bytes(b"CONFLICT_A")
        (self.base_b / "reshade-addons").mkdir(parents=True, exist_ok=True)
        (self.base_b / "reshade-addons/conflict.addon64").write_bytes(b"CONFLICT_B")

        res_b = core.install_mount(self.src_b, self.win64, self.wwmi, self.base_b, platform_kind="linux-proton",
                                   staging_root=self.staging, check_processes=False)

        # ReShade.ini preserved without blind replacement, and both stale-abs & relative warnings emitted
        merged_rs = (self.base_b / "ReShade.ini").read_text(encoding="utf-8")
        self.assertIn("LoadGameDirAddons=1", merged_rs)
        self.assertIn(f"ExtraAddonPaths={old_eng}\\custom-addons", merged_rs)
        self.assertIn("PresetPath=.\\Presets\\MyLook.ini", merged_rs)
        self.assertTrue(any("ExtraAddonPaths" in w for w in res_b["migration_warnings"]))
        self.assertTrue(any("PresetPath" in w for w in res_b["migration_warnings"]))

        # wwmi_dx12.ini kept UTF-16LE BOM + CRLF + user keys, migrated runtime_dir to B
        raw_b_ini = (self.base_b / "wwmi_dx12.ini").read_bytes()
        self.assertTrue(raw_b_ini.startswith(b"\xff\xfe"))
        text_b_ini = raw_b_ini[2:].decode("utf-16-le")
        self.assertIn("\r\n", text_b_ini)
        self.assertNotIn("\n", text_b_ini.replace("\r\n", ""))
        cp = configparser.ConfigParser(interpolation=None)
        cp.read_string(text_b_ini)
        self.assertEqual(cp["wwmi-dx12"]["debug"], "1")
        self.assertEqual(cp["wwmi-dx12"]["skin_mode"], "exact")
        self.assertEqual(cp["wwmi-dx12"]["custom_flag"], "42")
        self.assertEqual(cp["wwmi-dx12"]["runtime_dir"], core.format_engine_path(self.base_b / "runtime", "linux-proton"))

        # Cache copied when absent/newer, skipped when B already newer
        self.assertEqual((self.base_b / "runtime/cache/mod_tex_adapt.cache").read_bytes(), b"ADAPT_CACHE_A")
        self.assertEqual(b_newer.read_bytes(), b"NEWER_LOD_IN_B")
        self.assertEqual(res_b["migrated_cache"], ["mod_tex_adapt.cache"])

        # Third-party addon copied, WWMI-owned addons come from B (not A), conflict keeps B + warns
        self.assertEqual((self.base_b / "reshade-addons/renodx-ww.addon64").read_bytes(), b"RENODX_A")
        self.assertEqual((self.base_b / "reshade-addons/conflict.addon64").read_bytes(), b"CONFLICT_B")
        self.assertEqual(res_b["migrated_addons"], ["renodx-ww.addon64"])
        self.assertTrue(any("conflict.addon64" in w for w in res_b["migration_warnings"]))

    def test_linux_proton_reverse_mapping_migrate_from_and_proc_detection(self) -> None:
        # Only Z:\... is reverse-mapped on Linux; arbitrary drive letters return None
        self.assertEqual(core.resolve_engine_path_to_host(r"Z:\srv\wwmi-a", "linux-proton"), Path("/srv/wwmi-a").resolve())
        self.assertIsNone(core.resolve_engine_path_to_host(r"D:\Tools\wwmi-a", "linux-proton"))
        self.assertIsNone(core.resolve_engine_path_to_host(r"C:\Tools\wwmi-a", "linux-proton"))
        self.assertIsNone(core.resolve_engine_path_to_host(r"Z:\srv\..\etc", "linux-proton"))

        # --migrate-from fallback when Win64/ReShade.ini uses a non-Z:\ drive letter
        core.install_mount(self.src_a, self.win64, self.wwmi, self.base_a, platform_kind="linux-proton",
                           staging_root=self.staging, check_processes=False)
        non_z_stub = b"[INSTALL]\nBasePath=D:\\CustomMappedDrive\\InstallA\n"
        (self.win64 / "ReShade.ini").write_bytes(non_z_stub)
        led_a_path = self.base_a / "runtime/install-ledger.json"
        led_a = json.loads(led_a_path.read_text(encoding="utf-8"))
        for rec in led_a["win64_files"]:
            if rec["relative"] == "ReShade.ini":
                rec["sha256"] = core.sha256_file(self.win64 / "ReShade.ini")
                rec["size"] = len(non_z_stub)
        led_a_path.write_text(json.dumps(led_a), encoding="utf-8")

        # Without --migrate-from: rejected with hint
        with self.assertRaisesRegex(RuntimeError, "--migrate-from"):
            core.install_mount(self.src_b, self.win64, self.wwmi, self.base_b, platform_kind="linux-proton",
                               staging_root=self.staging, check_processes=False)
        # With --migrate-from pointing to A: verified and succeeds!
        res = core.install_mount(self.src_b, self.win64, self.wwmi, self.base_b, platform_kind="linux-proton",
                                 staging_root=self.staging, check_processes=False, migrate_from=self.base_a)
        self.assertEqual(res["state"], "installed")
        self.assertEqual(res["upgraded_from"], str(self.base_a))

        # /proc Proton multi-token process detection
        fake_proc = self.case / "fake_proc"
        (fake_proc / "1001").mkdir(parents=True)
        (fake_proc / "1001/cmdline").write_bytes(
            b"/usr/lib/proton/wine64-preloader\x00Z:\\Games\\Client\\Binaries\\Win64\\Client-Win64-Shipping.exe\x00-dx12\x00"
        )
        (fake_proc / "1002").mkdir(parents=True)
        (fake_proc / "1002/cmdline").write_bytes(
            b"/usr/bin/python3\x00/steam/proton\x00waitforexitandrun\x00/steam/Wuthering Waves/Wuthering Waves.exe\x00"
        )
        (fake_proc / "1003").mkdir(parents=True)
        (fake_proc / "1003/cmdline").write_bytes(
            b"Z:\\Games\\Client-Win64-Shipping.exe -dx12\x00"
        )
        (fake_proc / "1004").mkdir(parents=True)
        (fake_proc / "1004/cmdline").write_bytes(b"/bin/bash\x00-c\x00echo Client-Win64-Shipping.exe.bak\x00")
        hits = sorted(core.running_watched_processes(proc_root=fake_proc))
        self.assertEqual(hits, [
            "1001:Client-Win64-Shipping.exe",
            "1002:Wuthering Waves.exe",
            "1003:Client-Win64-Shipping.exe",
        ])

    def test_semver_and_mocked_github_release_check(self) -> None:
        self.assertTrue(core.is_newer_version("v1.0.4", "v1.0.5"))
        self.assertTrue(core.is_newer_version("1.0.5-rc.1", "v1.0.5"))
        self.assertTrue(core.is_newer_version("v1.0.5-rc.1", "v1.0.5-rc.2"))
        self.assertFalse(core.is_newer_version("v1.0.5", "v1.0.5"))
        self.assertFalse(core.is_newer_version("v1.0.6", "v1.0.5"))
        self.assertFalse(core.is_newer_version("dev-8537c6b", "v1.0.5"))
        self.assertIsNone(core.parse_semver("dev-8537c6b"))

        payload = {
            "tag_name": "v1.0.5",
            "html_url": "https://github.com/Bud-Open/wwmi-dx12-rt/releases/tag/v1.0.5",
            "published_at": "2026-11-28T00:00:00Z",
            "assets": [
                {
                    "name": "WWMI-DX12-RT-win64.zip",
                    "browser_download_url": "https://github.com/Bud-Open/wwmi-dx12-rt/releases/download/v1.0.5/WWMI-DX12-RT-win64.zip",
                    "digest": "sha256:aaaa",
                },
                {
                    "name": "WWMI-DX12-RT-linux.tar.gz",
                    "browser_download_url": "https://github.com/Bud-Open/wwmi-dx12-rt/releases/download/v1.0.5/WWMI-DX12-RT-linux.tar.gz",
                    "digest": "sha256:bbbb",
                },
            ],
        }

        class FakeResp(io.BytesIO):
            def __enter__(self):
                return self

            def __exit__(self, *_a):
                return False

        captured_reqs = []

        def fake_urlopen(req, timeout=None):
            captured_reqs.append((req.full_url, dict(req.header_items()), timeout))
            return FakeResp(json.dumps(payload).encode("utf-8"))

        info_lin = core.check_latest_release("v1.0.4", "linux-proton", urlopen_fn=fake_urlopen)
        self.assertTrue(info_lin["update_available"])
        self.assertEqual(info_lin["latest_version"], "v1.0.5")
        self.assertEqual(info_lin["asset_name"], "WWMI-DX12-RT-linux.tar.gz")
        self.assertEqual(info_lin["asset_digest"], "sha256:bbbb")
        self.assertEqual(info_lin["release_url"], "https://github.com/Bud-Open/wwmi-dx12-rt/releases/tag/v1.0.5")
        self.assertEqual(captured_reqs[0][0], core.GITHUB_RELEASE_API_URL)

        info_win = core.check_latest_release("v1.0.5", "windows", urlopen_fn=fake_urlopen)
        self.assertFalse(info_win["update_available"])
        self.assertEqual(info_win["asset_name"], "WWMI-DX12-RT-win64.zip")

        # Reject untrusted / spoofed release URLs by falling back to canonical GitHub Releases URL
        spoofed = dict(payload, html_url="https://evil.example/phish", assets=[
            {"name": "WWMI-DX12-RT-linux.tar.gz", "browser_download_url": "https://evil.example/evil.tar.gz"}
        ])
        parsed_spoof = core.parse_github_release_payload(spoofed, "v1.0.4", "linux-proton")
        self.assertEqual(parsed_spoof["release_url"], core.GITHUB_FALLBACK_RELEASE_URL)
        self.assertIsNone(parsed_spoof["asset_url"])

        # CLI `check-update --json` works cleanly with mocked check_latest_release
        setup = load_script("linux_setup_update", "scripts/wwmi-dx12-setup.py")
        buf = io.StringIO()
        with patch.object(setup, "REPO_ROOT", self.src_a), \
                patch.object(setup.core, "check_latest_release", return_value=info_lin), \
                patch.object(sys, "stdout", buf):
            rc = setup.main(["check-update", "--json"])
        self.assertEqual(rc, 0)
        cli_out = json.loads(buf.getvalue())
        self.assertEqual(cli_out["latest_version"], "v1.0.5")
        self.assertTrue(cli_out["update_available"])

    def test_crash_retry_reuses_original_backup_and_never_poisons_recovery(self) -> None:
        core.install_mount(self.src_a, self.win64, self.wwmi, self.base_a, platform_kind="linux-proton",
                           staging_root=self.staging, check_processes=False)
        a_dxgi = (self.win64 / "dxgi.dll").read_bytes()
        a_ini = (self.win64 / "ReShade.ini").read_bytes()
        a_dxgi_sha = core.sha256_file(self.win64 / "dxgi.dll")
        a_ini_sha = core.sha256_file(self.win64 / "ReShade.ini")

        rec_dir = self.base_b / "runtime/recovery/crash001"
        rec_dir.mkdir(parents=True, exist_ok=True)
        (rec_dir / "dxgi.dll").write_bytes(a_dxgi)
        (rec_dir / "ReShade.ini").write_bytes(a_ini)
        b_dxgi = (self.src_b / "host/ReShade64-game-forward.dll").read_bytes()
        b_ini = core.build_win64_stub_reshade_ini(core.format_engine_path(self.base_b, "linux-proton")).encode("utf-8")
        (self.win64 / "dxgi.dll").write_bytes(b_dxgi)  # mixed: B dxgi + A ReShade.ini
        led_b = {
            "schema_version": 1, "state": "installing", "pending_id": "a" * 32,
            "platform_kind": "linux-proton", "base_root": str(self.base_b),
            "win64_dir": str(self.win64), "wwmi_dir": str(self.wwmi),
            "previous_owner": str(self.base_a), "recovery_dir": str(rec_dir),
            "win64_files": [
                {"relative": "dxgi.dll", "path": str(self.win64 / "dxgi.dll"),
                 "sha256": core.hashlib.sha256(b_dxgi).hexdigest(), "size": len(b_dxgi),
                 "previous_sha256": a_dxgi_sha, "recovery_backup": str(rec_dir / "dxgi.dll")},
                {"relative": "ReShade.ini", "path": str(self.win64 / "ReShade.ini"),
                 "sha256": core.hashlib.sha256(b_ini).hexdigest(), "size": len(b_ini),
                 "previous_sha256": a_ini_sha, "recovery_backup": str(rec_dir / "ReShade.ini")},
            ],
            "extra_managed_files": [],
        }
        (self.base_b / "runtime/install-ledger.json").write_text(json.dumps(led_b), encoding="utf-8")

        orig_write = core._write_new_file

        def fail_second(path: Path, data: bytes) -> None:
            if path.name.endswith("-ReShade.ini.tmp"):
                raise OSError("retry second file failure")
            return orig_write(path, data)

        with patch.object(core, "_write_new_file", side_effect=fail_second):
            with self.assertRaisesRegex(OSError, "retry second file failure"):
                core.install_mount(self.src_b, self.win64, self.wwmi, self.base_b, platform_kind="linux-proton",
                                   staging_root=self.staging, check_processes=False)

        self.assertEqual((self.win64 / "dxgi.dll").read_bytes(), a_dxgi)
        self.assertEqual((self.win64 / "ReShade.ini").read_bytes(), a_ini)
        st_a, _ = core.inspect_mount_state(self.win64, self.base_a)
        self.assertEqual(st_a, "installed")

    def test_v103_legacy_upgrade_rollback_from_new_setup_and_inplace_refresh(self) -> None:
        core.install_mount(self.src_a, self.win64, self.wwmi, self.base_a, platform_kind="linux-proton",
                           staging_root=self.staging, check_processes=False)
        led_a_path = self.base_a / "runtime/install-ledger.json"
        led_a = json.loads(led_a_path.read_text(encoding="utf-8"))
        led_a.pop("installed_version", None)
        led_a.pop("installed_commit", None)
        led_a_path.write_text(json.dumps(led_a), encoding="utf-8")
        a_dxgi = (self.win64 / "dxgi.dll").read_bytes()
        a_ini = (self.win64 / "ReShade.ini").read_bytes()

        res_b = core.install_mount(self.src_b, self.win64, self.wwmi, self.base_b, platform_kind="linux-proton",
                                   staging_root=self.staging, check_processes=False)
        self.assertEqual(res_b["install_mode"], "upgrade")
        self.assertEqual(res_b["upgraded_from"], str(self.base_a))

        rb = core.rollback_mount(self.base_b, staging_root=self.staging, check_processes=False)
        self.assertEqual(rb["state"], "rolled_back")
        self.assertEqual(rb["to_root"], str(self.base_a))
        self.assertEqual((self.win64 / "dxgi.dll").read_bytes(), a_dxgi)
        self.assertEqual((self.win64 / "ReShade.ini").read_bytes(), a_ini)
        st_a, _ = core.inspect_mount_state(self.win64, self.base_a)
        self.assertEqual(st_a, "installed")
        self.assertEqual(json.loads((self.base_b / "runtime/install-ledger.json").read_text())["state"], "uninstalled")

        ini_a = self.base_a / "wwmi_dx12.ini"
        ini_a.write_bytes(
            (
                "[wwmi-dx12]\r\n"
                f"wwmi_dir = {core.format_engine_path(self.wwmi, 'linux-proton')}\r\n"
                f"runtime_dir = {core.format_engine_path(self.base_a / 'runtime', 'linux-proton')}\r\n"
                "hide_uid = 1\r\n"
            ).encode("utf-16")
        )
        res_ref = core.install_mount(self.src_b, self.win64, self.wwmi, self.base_a, platform_kind="linux-proton",
                                     staging_root=self.staging, check_processes=False)
        self.assertEqual(res_ref["install_mode"], "refresh")
        self.assertEqual(res_ref["installed_version"], "v1.0.5")
        self.assertEqual((self.win64 / "dxgi.dll").read_bytes(), (self.src_b / "host/ReShade64-game-forward.dll").read_bytes())
        vals = core.parse_wwmi_ini_values(core.decode_ini_bytes(ini_a.read_bytes()))
        self.assertEqual(vals.get("hide_uid"), "1")

    def test_env_independent_win64_lock_and_base_root_lock(self) -> None:
        with patch.dict(os.environ, {"XDG_CACHE_HOME": str(self.case / "cache_1")}):
            p1 = core._win64_lock_path(self.win64)
        with patch.dict(os.environ, {"XDG_CACHE_HOME": str(self.case / "cache_2")}):
            p2 = core._win64_lock_path(self.win64)
        self.assertEqual(p1, p2)

        with core._base_root_locks(self.base_a):
            with self.assertRaisesRegex(RuntimeError, "锁定|locked"):
                with core._base_root_locks(self.base_a):
                    pass

    def test_strict_stub_paired_host_unc_and_recovery_binding_rejections(self) -> None:
        self.assertIsNone(core.resolve_engine_path_to_host(r"\\evil-server\share\WWMI", "windows"))
        self.assertIsNone(core.resolve_engine_path_to_host(r"//evil-server/share/WWMI", "windows"))
        self.assertIsNone(core.resolve_engine_path_to_host(r"\\?\UNC\evil-server\share", "windows"))

        full_third_party = "[GENERAL]\nPerformanceMode=1\n[INSTALL]\nBasePath=Z:\\tmp\\fake\n"
        self.assertIsNone(core.parse_win64_stub_base_path(full_third_party, strict=True))
        self.assertEqual(core.parse_win64_stub_base_path("[INSTALL]\nBasePath=Z:\\tmp\\ok\n", strict=True), "Z:\\tmp\\ok")

        core.install_mount(self.src_a, self.win64, self.wwmi, self.base_a, platform_kind="linux-proton",
                           staging_root=self.staging, check_processes=False)
        (self.win64 / "dxgi.dll").write_bytes(b"THIRD_PARTY_DXGI")
        led_a_path = self.base_a / "runtime/install-ledger.json"
        led_a = json.loads(led_a_path.read_text(encoding="utf-8"))
        for rec in led_a["win64_files"]:
            if rec["relative"] == "dxgi.dll":
                rec["sha256"] = core.sha256_file(self.win64 / "dxgi.dll")
        led_a_path.write_text(json.dumps(led_a), encoding="utf-8")
        owner, reason = core.detect_verified_win64_owner(self.win64, self.base_b, "linux-proton")
        self.assertIsNone(owner)
        self.assertIsNotNone(reason)

        fake_b = self.case / "FakeInstallingB"
        fake_b.mkdir()
        (fake_b / "runtime").mkdir()
        forged = {
            "schema_version": 1, "state": "installing", "pending_id": "b" * 32,
            "platform_kind": "linux-proton", "base_root": str(fake_b),
            "win64_dir": str(self.win64), "wwmi_dir": str(self.wwmi),
            "previous_owner": str(self.base_a),
            "win64_files": [
                {"relative": "dxgi.dll", "path": str(self.win64 / "dxgi.dll"),
                 "sha256": "0" * 64, "previous_sha256": core.sha256_file(self.win64 / "dxgi.dll")},
                {"relative": "ReShade.ini", "path": str(self.win64 / "ReShade.ini"),
                 "sha256": "1" * 64, "previous_sha256": core.sha256_file(self.win64 / "ReShade.ini")},
            ],
        }
        with self.assertRaisesRegex(RuntimeError, "哈希|hash"):
            core._owned_bootstrap(self.win64, forged, base_root=fake_b)

    def test_active_wwmi_ini_takes_precedence_over_stale_ledger_wwmi_dir(self) -> None:
        core.install_mount(self.src_a, self.win64, self.wwmi, self.base_a, platform_kind="linux-proton",
                           staging_root=self.staging, check_processes=False)
        wwmi_new = core.init_wwmi_skeleton(self.case / "WWMI_NEW")
        (wwmi_new / "Mods/Skin").mkdir(parents=True, exist_ok=True)
        (wwmi_new / "Mods/Skin/mod.ini").write_text("[Constants]\n", encoding="utf-8")
        (self.base_a / "wwmi_dx12.ini").write_bytes(
            (
                "[wwmi-dx12]\r\n"
                f"wwmi_dir = {core.format_engine_path(wwmi_new, 'linux-proton')}\r\n"
                f"runtime_dir = {core.format_engine_path(self.base_a / 'runtime', 'linux-proton')}\r\n"
            ).encode("utf-16")
        )
        res_b = core.install_mount(
            self.src_b, self.win64, self.base_b / "WWMI", self.base_b, platform_kind="linux-proton",
            staging_root=self.staging, check_processes=False, init_missing_wwmi=True,
        )
        self.assertEqual(Path(res_b["wwmi_dir"]).resolve(), wwmi_new.resolve())
        vals = core.parse_wwmi_ini_values(core.decode_ini_bytes((self.base_b / "wwmi_dx12.ini").read_bytes()))
        self.assertEqual(vals.get("wwmi_dir"), core.format_engine_path(wwmi_new, "linux-proton"))

    def test_same_root_refresh_preserves_upgraded_from_for_default_rollback(self) -> None:
        core.install_mount(self.src_a, self.win64, self.wwmi, self.base_a, platform_kind="linux-proton",
                           staging_root=self.staging, check_processes=False)
        core.install_mount(self.src_b, self.win64, self.wwmi, self.base_b, platform_kind="linux-proton",
                           staging_root=self.staging, check_processes=False)
        # Re-running Install on B (same-root refresh) MUST preserve upgraded_from -> A.
        res_ref = core.install_mount(self.src_b, self.win64, self.wwmi, self.base_b, platform_kind="linux-proton",
                                     staging_root=self.staging, check_processes=False)
        self.assertEqual(res_ref["install_mode"], "refresh")
        self.assertEqual(res_ref.get("upgraded_from"), str(self.base_a))
        # Default rollback without --to MUST still succeed.
        rb = core.rollback_mount(self.base_b, staging_root=self.staging, check_processes=False)
        self.assertEqual(rb["state"], "rolled_back")
        self.assertEqual(rb["to_root"], str(self.base_a))

    def test_rollback_crash_recovery_resumes_mid_swap_and_post_ledger_a(self) -> None:
        core.install_mount(self.src_a, self.win64, self.wwmi, self.base_a, platform_kind="linux-proton",
                           staging_root=self.staging, check_processes=False)
        a_dxgi = (self.win64 / "dxgi.dll").read_bytes()
        a_ini = (self.win64 / "ReShade.ini").read_bytes()
        core.install_mount(self.src_b, self.win64, self.wwmi, self.base_b, platform_kind="linux-proton",
                           staging_root=self.staging, check_processes=False)
        b_dxgi = (self.win64 / "dxgi.dll").read_bytes()
        b_ini = (self.win64 / "ReShade.ini").read_bytes()

        # Case 1: Rollback crashes after writing both Win64 files and before writing A's ledger.
        orig_write_json = core._write_json
        def fail_write_a_ledger(path: Path, obj: dict) -> None:
            if path.resolve() == (self.base_a / "runtime/install-ledger.json").resolve():
                raise OSError("crash before ledger A")
            return orig_write_json(path, obj)

        with patch.object(core, "_write_json", side_effect=fail_write_a_ledger):
            with self.assertRaisesRegex(OSError, "crash before ledger A"):
                core.rollback_mount(self.base_b, staging_root=self.staging, check_processes=False)

        # B is in rolling_back; Win64 already has A's files; install/uninstall refuse; retrying rollback succeeds.
        self.assertEqual(json.loads((self.base_b / "runtime/install-ledger.json").read_text())["state"], "rolling_back")
        with self.assertRaisesRegex(RuntimeError, "回滚|rollback"):
            core.uninstall_mount(self.base_b, staging_root=self.staging, check_processes=False)

        # Case 2: Retry writes A's ledger, then crashes right before marking B uninstalled -> next retry finishes cleanly.
        def fail_write_b_final(path: Path, obj: dict) -> None:
            if path.resolve() == (self.base_b / "runtime/install-ledger.json").resolve() and obj.get("state") == "uninstalled":
                raise OSError("crash before ledger B uninstalled")
            return orig_write_json(path, obj)

        with patch.object(core, "_write_json", side_effect=fail_write_b_final):
            with self.assertRaisesRegex(OSError, "crash before ledger B uninstalled"):
                core.rollback_mount(self.base_b, staging_root=self.staging, check_processes=False)
        self.assertEqual(json.loads((self.base_a / "runtime/install-ledger.json").read_text())["state"], "installed")
        self.assertEqual(json.loads((self.base_b / "runtime/install-ledger.json").read_text())["state"], "rolling_back")

        rb = core.rollback_mount(self.base_b, staging_root=self.staging, check_processes=False)
        self.assertEqual(rb["state"], "rolled_back")
        self.assertEqual((self.win64 / "dxgi.dll").read_bytes(), a_dxgi)
        self.assertEqual((self.win64 / "ReShade.ini").read_bytes(), a_ini)
        self.assertEqual(json.loads((self.base_b / "runtime/install-ledger.json").read_text())["state"], "uninstalled")
        self.assertNotEqual(a_dxgi, b_dxgi)
        self.assertNotEqual(a_ini, b_ini)

    def test_installing_retry_with_new_payload_b2_resets_b1_before_ledger_update(self) -> None:
        core.install_mount(self.src_a, self.win64, self.wwmi, self.base_a, platform_kind="linux-proton",
                           staging_root=self.staging, check_processes=False)
        a_dxgi = (self.win64 / "dxgi.dll").read_bytes()
        a_ini = (self.win64 / "ReShade.ini").read_bytes()
        b1_dxgi = (self.src_b / "host/ReShade64-game-forward.dll").read_bytes()

        # Simulate hard crash mid-upgrade after B1 replaced dxgi.dll (ReShade.ini still A)
        rec_dir = self.base_b / "runtime/recovery/batch-b1-crash"
        rec_dir.mkdir(parents=True, exist_ok=True)
        (rec_dir / "dxgi.dll").write_bytes(a_dxgi)
        (rec_dir / "ReShade.ini").write_bytes(a_ini)
        (self.win64 / "dxgi.dll").write_bytes(b1_dxgi)
        led_b1 = {
            "schema_version": 1, "state": "installing", "pending_id": "c" * 32,
            "platform_kind": "linux-proton", "base_root": str(self.base_b),
            "win64_dir": str(self.win64), "wwmi_dir": str(self.wwmi),
            "previous_owner": str(self.base_a), "recovery_dir": str(rec_dir),
            "win64_files": [
                {"relative": "dxgi.dll", "path": str(self.win64 / "dxgi.dll"),
                 "sha256": core.hashlib.sha256(b1_dxgi).hexdigest(), "size": len(b1_dxgi),
                 "previous_sha256": core.hashlib.sha256(a_dxgi).hexdigest(),
                 "recovery_backup": str(rec_dir / "dxgi.dll")},
                {"relative": "ReShade.ini", "path": str(self.win64 / "ReShade.ini"),
                 "sha256": core.hashlib.sha256(core.build_win64_stub_reshade_ini(core.format_engine_path(self.base_b, "linux-proton")).encode()).hexdigest(),
                 "size": len(a_ini), "previous_sha256": core.hashlib.sha256(a_ini).hexdigest(),
                 "recovery_backup": str(rec_dir / "ReShade.ini")},
            ],
            "extra_managed_files": [],
        }
        (self.base_b / "runtime/install-ledger.json").write_text(json.dumps(led_b1), encoding="utf-8")

        # Now update payload in src_b to B2 and crash right AFTER B2 ledger write and BEFORE first swap.
        src_b2 = populate_source(self.case / "SrcB2", "v1.0.6", "deadbeef", b"DXGI_V106_B2_PAYLOAD")
        orig_write_new = core._write_new_file
        def fail_first_swap(path: Path, data: bytes) -> None:
            if path.parent.resolve() == self.win64.resolve() and path.name.endswith("-dxgi.dll.tmp"):
                raise OSError("crash before B2 first swap")
            return orig_write_new(path, data)

        with patch.object(core, "_write_new_file", side_effect=fail_first_swap):
            with self.assertRaisesRegex(OSError, "crash before B2 first swap"):
                core.install_mount(src_b2, self.win64, self.wwmi, self.base_b, platform_kind="linux-proton",
                                   staging_root=self.staging, check_processes=False)

        # Because B1 was reset to verified A BEFORE writing B2's ledger, Win64 matches A and B2 retry succeeds!
        self.assertEqual((self.win64 / "dxgi.dll").read_bytes(), a_dxgi)
        res_ok = core.install_mount(src_b2, self.win64, self.wwmi, self.base_b, platform_kind="linux-proton",
                                    staging_root=self.staging, check_processes=False)
        self.assertEqual(res_ok["state"], "installed")
        self.assertEqual((self.win64 / "dxgi.dll").read_bytes(), b"DXGI_V106_B2_PAYLOAD")

    def test_rollback_revalidates_target_a_after_acquiring_locks(self) -> None:
        core.install_mount(self.src_a, self.win64, self.wwmi, self.base_a, platform_kind="linux-proton",
                           staging_root=self.staging, check_processes=False)
        core.install_mount(self.src_b, self.win64, self.wwmi, self.base_b, platform_kind="linux-proton",
                           staging_root=self.staging, check_processes=False)
        win64_2 = make_game(self.case / "Game2")

        orig_lock = core._base_root_locks
        rebound = {"done": False}

        @core.contextlib.contextmanager
        def rebind_a_before_lock(*roots):
            if not rebound["done"]:
                rebound["done"] = True
                # Simulate another process installing A to Game2 right before rollback acquires locks.
                core.install_mount(self.src_a, win64_2, self.wwmi, self.base_a, platform_kind="linux-proton",
                                   staging_root=self.staging, check_processes=False)
            with orig_lock(*roots):
                yield

        with patch.object(core, "_base_root_locks", side_effect=rebind_a_before_lock):
            with self.assertRaisesRegex(RuntimeError, "活动安装|actively installed|不一致|match"):
                core.rollback_mount(self.base_b, staging_root=self.staging, check_processes=False)

        # A MUST remain cleanly bound to Game2; Win64 #1 still cleanly owned by B.
        self.assertEqual(Path(json.loads((self.base_a / "runtime/install-ledger.json").read_text())["win64_dir"]).resolve(), win64_2.resolve())
        st_b, _ = core.inspect_mount_state(self.win64, self.base_b)
        self.assertEqual(st_b, "installed")

    def test_windows_lock_fold_and_remote_drive_rejection(self) -> None:
        # Must NOT expand U+0130 into two codepoints like Python str.lower().
        folded = core._fold_windows_lock_path("C:/Mods/İstanbul\\")
        self.assertEqual(folded, "C:\\MODS\\İSTANBUL")
        self.assertNotIn("\u0307", folded)

        # Mapped network drive (DRIVE_REMOTE) must be rejected BEFORE Path.resolve().
        with patch.object(core, "_is_local_windows_drive", return_value=False):
            with patch.object(core, "Path") as mock_path:
                self.assertIsNone(core.resolve_engine_path_to_host(r"X:\WWMI", "windows"))
                mock_path.assert_not_called()

    def test_rollback_swap_failure_restores_b_ledger_on_first_or_locked_second_file(self) -> None:
        core.install_mount(self.src_a, self.win64, self.wwmi, self.base_a, platform_kind="linux-proton",
                           staging_root=self.staging, check_processes=False)
        core.install_mount(self.src_b, self.win64, self.wwmi, self.base_b, platform_kind="linux-proton",
                           staging_root=self.staging, check_processes=False)
        b_dxgi = (self.win64 / "dxgi.dll").read_bytes()
        b_ini = (self.win64 / "ReShade.ini").read_bytes()

        # Case A: First file replace fails -> B must restore to `installed` immediately.
        orig_replace = core.os.replace
        def fail_first_replace(src, dst):
            if Path(dst) == self.win64 / "dxgi.dll":
                raise PermissionError("dxgi locked")
            return orig_replace(src, dst)

        with patch.object(core.os, "replace", side_effect=fail_first_replace):
            with self.assertRaisesRegex(PermissionError, "dxgi locked"):
                core.rollback_mount(self.base_b, staging_root=self.staging, check_processes=False)
        self.assertEqual(json.loads((self.base_b / "runtime/install-ledger.json").read_text())["state"], "installed")
        self.assertEqual((self.win64 / "dxgi.dll").read_bytes(), b_dxgi)
        self.assertEqual((self.win64 / "ReShade.ini").read_bytes(), b_ini)

        # Case B: Second file replace fails (locked while unchanged) -> restore skips unchanged file & restores B `installed`.
        def fail_second_replace(src, dst):
            if Path(dst) == self.win64 / "ReShade.ini":
                raise PermissionError("ini locked")
            return orig_replace(src, dst)

        with patch.object(core.os, "replace", side_effect=fail_second_replace):
            with self.assertRaisesRegex(PermissionError, "ini locked"):
                core.rollback_mount(self.base_b, staging_root=self.staging, check_processes=False)
        self.assertEqual(json.loads((self.base_b / "runtime/install-ledger.json").read_text())["state"], "installed")
        self.assertEqual((self.win64 / "dxgi.dll").read_bytes(), b_dxgi)
        self.assertEqual((self.win64 / "ReShade.ini").read_bytes(), b_ini)

    def test_rollback_blocks_uninstalling_a_until_b_finalizes_and_handles_stale_installed_a(self) -> None:
        # Subtest 1: Upgrade succeeded, but marking A migrated_to failed (A still says `installed` on SAME Win64).
        core.install_mount(self.src_a, self.win64, self.wwmi, self.base_a, platform_kind="linux-proton",
                           staging_root=self.staging, check_processes=False)
        with patch.object(core, "_mark_previous_owner_migrated", return_value="readonly A warning"):
            res_b = core.install_mount(self.src_b, self.win64, self.wwmi, self.base_b, platform_kind="linux-proton",
                                       staging_root=self.staging, check_processes=False)
        self.assertEqual(res_b["state"], "installed")
        self.assertEqual(json.loads((self.base_a / "runtime/install-ledger.json").read_text())["state"], "installed")

        # Default rollback on B MUST succeed against stale A ledger on same Win64; inject crash before B finalizes.
        orig_write_json = core._write_json
        def fail_b_finalize(path: Path, obj: dict) -> None:
            if path.resolve() == (self.base_b / "runtime/install-ledger.json").resolve() and obj.get("state") == "uninstalled":
                raise OSError("crash before B finalize")
            return orig_write_json(path, obj)

        with patch.object(core, "_write_json", side_effect=fail_b_finalize):
            with self.assertRaisesRegex(OSError, "crash before B finalize"):
                core.rollback_mount(self.base_b, staging_root=self.staging, check_processes=False)

        # Subtest 2: Uninstalling A or rolling back A->C before B finalizes MUST be refused, and corrupt B ledger MUST NOT bypass guard.
        with self.assertRaisesRegex(RuntimeError, "回滚收尾|finalization"):
            core.uninstall_mount(self.base_a, staging_root=self.staging, check_processes=False)
        base_c = self.case / "BaseC"
        populate_source(base_c, "v1.0.2", "00001111", b"HOST_C_PAYLOAD")
        (base_c / "wwmi_dx12.ini").write_text("[wwmi-dx12]\n", encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "回滚收尾|finalization"):
            core.rollback_mount(self.base_a, target_root=base_c, staging_root=self.staging, check_processes=False)

        b_led_p = self.base_b / "runtime/install-ledger.json"
        saved_b_led = b_led_p.read_text(encoding="utf-8")
        b_led_p.write_text("{corrupt", encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "损坏|corrupted"):
            core.uninstall_mount(self.base_a, staging_root=self.staging, check_processes=False)
        b_led_p.write_text(saved_b_led, encoding="utf-8")

        rb_fin = core.rollback_mount(self.base_b, staging_root=self.staging, check_processes=False)
        self.assertEqual(rb_fin["state"], "rolled_back")
        self.assertEqual(json.loads(b_led_p.read_text())["state"], "uninstalled")
        un_a = core.uninstall_mount(self.base_a, staging_root=self.staging, check_processes=False)
        self.assertEqual(un_a["state"], "uninstalled")

    def test_stale_a_marked_inactive_before_rolling_back_so_mid_swap_crash_resumes(self) -> None:
        core.install_mount(self.src_a, self.win64, self.wwmi, self.base_a, platform_kind="linux-proton",
                           staging_root=self.staging, check_processes=False)
        with patch.object(core, "_mark_previous_owner_migrated", return_value="readonly A warning"):
            core.install_mount(self.src_b, self.win64, self.wwmi, self.base_b, platform_kind="linux-proton",
                               staging_root=self.staging, check_processes=False)
        self.assertEqual(json.loads((self.base_a / "runtime/install-ledger.json").read_text())["state"], "installed")

        # Crash right before committing A's restored ledger (simulating hard power loss after both files swapped).
        orig_write_json = core._write_json
        def crash_commit_a(path: Path, obj: dict) -> None:
            if path.resolve() == (self.base_a / "runtime/install-ledger.json").resolve() and obj.get("state") == "installed":
                raise OSError("power loss before A commit")
            return orig_write_json(path, obj)

        with patch.object(core, "_write_json", side_effect=crash_commit_a):
            with self.assertRaisesRegex(OSError, "power loss before A commit"):
                core.rollback_mount(self.base_b, staging_root=self.staging, check_processes=False)

        # Stale A was already marked uninstalled before B started rolling_back, so retrying B resumes cleanly!
        self.assertEqual(json.loads((self.base_a / "runtime/install-ledger.json").read_text())["state"], "uninstalled")
        self.assertEqual(json.loads((self.base_b / "runtime/install-ledger.json").read_text())["state"], "rolling_back")
        rb_ok = core.rollback_mount(self.base_b, staging_root=self.staging, check_processes=False)
        self.assertEqual(rb_ok["state"], "rolled_back")
        self.assertEqual(json.loads((self.base_a / "runtime/install-ledger.json").read_text())["state"], "installed")

    def test_deleted_restored_from_dir_is_never_recreated_by_subsequent_rollback(self) -> None:
        core.install_mount(self.src_a, self.win64, self.wwmi, self.base_a, platform_kind="linux-proton",
                           staging_root=self.staging, check_processes=False)
        core.install_mount(self.src_b, self.win64, self.wwmi, self.base_b, platform_kind="linux-proton",
                           staging_root=self.staging, check_processes=False)
        core.rollback_mount(self.base_b, staging_root=self.staging, check_processes=False)

        # Case 1: Only B/runtime removed (B itself remains) -> must NOT recreate B/runtime during A -> C rollback.
        moved_rt = self.case / "MovedBRuntime"
        (self.base_b / "runtime").rename(moved_rt)
        self.assertTrue(self.base_b.is_dir())
        self.assertFalse((self.base_b / "runtime").exists())
        base_c = self.case / "BaseC"
        populate_source(base_c, "v1.0.2", "00001111", b"HOST_C_PAYLOAD")
        (base_c / "wwmi_dx12.ini").write_text("[wwmi-dx12]\n", encoding="utf-8")

        rb_ac = core.rollback_mount(self.base_a, target_root=base_c, staging_root=self.staging, check_processes=False)
        self.assertEqual(rb_ac["state"], "rolled_back")
        self.assertFalse((self.base_b / "runtime").exists())

        # Case 2: Whole B removed -> must NOT recreate B either.
        moved_b = self.case / "MovedAwayB"
        self.base_b.rename(moved_b)
        self.assertFalse(self.base_b.exists())
        self.assertIsNone(core._restored_source_lock_target(
            {"restored_from": str(self.base_b), "restored_pending_id": "a" * 32}, self.base_a,
        ))


if __name__ == "__main__":
    unittest.main()
