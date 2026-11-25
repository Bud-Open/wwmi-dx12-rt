import configparser
import importlib.util
import json
import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path, PurePosixPath, PureWindowsPath
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


class TestInstallerCore(unittest.TestCase):
    def setUp(self) -> None:
        self.case = Path(tempfile.mkdtemp(prefix="wwmi-installer-test-")).resolve()
        self.game_root = self.case / "Game/Wuthering Waves Game"
        self.win64 = make_game(self.game_root)
        self.wwmi = core.init_wwmi_skeleton(self.case / "中文模组")
        self.base = self.case / "外部运行时"
        self.journal = self.base / "runtime/install-ledger.json"
        self.staging = self.case / "Staging"
        self.source = self.case / "Source"
        for name in (*core.REQUIRED_PAYLOAD, "lib/wine-filename-utf8.so"):
            target = self.source / name
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes(name.encode("ascii"))

    def install(self, **overrides):
        kwargs = dict(source_root=self.source, win64_dir=self.win64, wwmi_dir=self.wwmi,
                      base_root=self.base, staging_root=self.staging, check_processes=False)
        return core.install_mount(**(kwargs | overrides))

    def uninstall(self, **overrides):
        kwargs = dict(base_root=self.base, staging_root=self.staging, check_processes=False)
        return core.uninstall_mount(**(kwargs | overrides))

    def stage(self, *files, label="fixture"):
        return core.stage_files_for_removal([self.win64 / f for f in files], self.staging, label)

    def state(self, win64=None):
        return core.inspect_mount_state(win64 or self.win64, self.base)[0]

    def test_engine_paths(self) -> None:
        win = PureWindowsPath(r"D:\Games\中文模组")
        linux = PurePosixPath("/srv/Mods/中文模组")
        self.assertEqual(core.format_engine_path(win, platform_kind="windows"), str(win))
        self.assertEqual(core.format_engine_path(linux, platform_kind="linux-proton"), r"Z:\srv\Mods\中文模组")

    def test_vdf_parse_and_flatpak_discovery(self) -> None:
        fake_home = self.case / "Home"
        fp_steam = fake_home / ".var/app/com.valvesoftware.Steam/.local/share/Steam"
        fp_data = fake_home / ".var/app/com.valvesoftware.Steam/data"
        library = fake_home / "mnt_nvme2/SteamLibrary"

        (fp_steam / "steamapps").mkdir(parents=True)
        fp_data.mkdir(parents=True)
        win64 = make_game(library / "steamapps/common/Wuthering Waves/Wuthering Waves Game")
        compat = library / "steamapps/compatdata/3513350"
        compat.mkdir(parents=True)

        quoted = str(library).replace("\\", "\\\\")
        (fp_steam / "steamapps/libraryfolders.vdf").write_text(
            f'"libraryfolders"\n{{\n "1"\n {{\n "path" "{quoted}"\n }}\n}}\n', encoding="utf-8",
        )
        installs = core.discover_linux_game_installs(home=fake_home)
        self.assertEqual(len(installs), 1)
        inst = installs[0]
        self.assertTrue(inst.is_flatpak_steam)
        self.assertEqual(inst.win64_dir, win64.resolve())
        self.assertEqual(inst.compatdata_dir, compat)
        self.assertEqual(core.select_default_linux_base_root(inst, home=fake_home), fp_data / "wwmi-dx12-rt")

    def test_install_idempotent_conflict_guard_and_safe_uninstall(self) -> None:
        (self.wwmi / "Core/WWMI").mkdir()
        (self.wwmi / "Core/WWMI/WuWa-Model-Importer.ini").touch()
        rabbit = self.wwmi / "Mods/测试/RabbitFX.ini"
        rabbit.parent.mkdir()
        rabbit.touch()

        (self.win64 / "dxgi.dll").write_bytes(b"FOREIGN")
        with self.assertRaises(RuntimeError):
            self.install()
        self.assertFalse(self.base.exists())
        self.stage("dxgi.dll")

        installed = self.install()
        self.assertEqual(installed["state"], "installed")
        self.assertCountEqual(os.listdir(self.win64), [core.TARGET_SHIPPING_EXE, *core.BOOTSTRAP_FILES])
        rep = core.run_doctor(self.win64, self.wwmi, self.base)
        self.assertTrue(rep.healthy)
        self.assertEqual(rep.mount_state, "installed")
        self.assertEqual(rep.rabbitfx_count, 1)
        self.assertEqual(rep.non_ascii_mods, ["测试"])
        dup = self.wwmi / "Mods/ExtraCopy/RabbitFX.ini"
        dup.parent.mkdir()
        dup.touch()
        rep = core.run_doctor(self.win64, self.wwmi, self.base)
        self.assertFalse(rep.healthy)
        self.assertEqual(rep.rabbitfx_count, 2)

        self.assertEqual(self.install()["state"], "installed")
        result = self.uninstall()
        self.assertEqual(result["state"], "uninstalled")
        self.assertEqual(os.listdir(self.win64), [core.TARGET_SHIPPING_EXE])
        manifest = json.loads((Path(result["staged_batch"]) / "manifest.json").read_text(encoding="utf-8"))
        self.assertEqual(len(manifest["entries"]), 2)
        for entry in manifest["entries"]:
            self.assertTrue(Path(entry["staged_path"]).is_file())
        self.assertEqual(self.uninstall()["state"], "noop")

    def test_conflict_wins_over_missing_file_in_both_orders(self) -> None:
        for missing, foreign in (core.BOOTSTRAP_FILES, core.BOOTSTRAP_FILES[::-1]):
            with self.subTest(missing=missing):
                self.install()
                batch = self.stage(missing, foreign)
                (self.win64 / foreign).write_bytes(b"third-party")
                self.assertEqual(self.state(), "conflict")
                with self.assertRaisesRegex(RuntimeError, "哈希"):
                    self.install()
                with self.assertRaisesRegex(RuntimeError, "哈希"):
                    self.uninstall()
                self.assertEqual((self.win64 / foreign).read_bytes(), b"third-party")
                self.stage(foreign)
                # Put back only the unmodified surviving file; now the gap is safe to repair.
                shutil.move(batch / f"001_{foreign}", self.win64 / foreign)
                self.assertEqual(self.state(), "drifted")
                self.assertEqual(self.install()["state"], "installed")

    def test_non_regular_or_dangling_bootstrap_is_conflict(self) -> None:
        dxgi = self.win64 / "dxgi.dll"
        dxgi.mkdir()
        self.assertEqual(self.state(), "conflict")
        with self.assertRaisesRegex(RuntimeError, "非普通文件"):
            self.install()
        self.stage("dxgi.dll")
        if sys.platform != "win32":  # Windows symlink creation may need privileges.
            dxgi.symlink_to(self.case / "missing")
            self.assertEqual(self.state(), "conflict")

    def test_active_mount_blocks_other_game_even_if_both_files_missing(self) -> None:
        self.install()
        other = make_game(self.case / "GameB")
        self.stage(*core.BOOTSTRAP_FILES)
        self.assertEqual(self.state(), "drifted")
        self.assertEqual(self.state(other), "active_elsewhere")
        before = self.journal.read_bytes()
        with self.assertRaisesRegex(RuntimeError, "先卸载"):
            self.install(win64_dir=other)
        self.assertEqual(self.journal.read_bytes(), before)
        self.assertEqual(os.listdir(other), [core.TARGET_SHIPPING_EXE])
        self.uninstall()
        self.assertEqual(self.install(win64_dir=other)["state"], "installed")

    def test_base_root_validation_has_no_side_effects(self) -> None:
        targets = [self.game_root, self.game_root / "Inside", self.win64, self.win64 / "Inside",
                   self.wwmi / "Inside", self.case / "comma,folder"]
        if sys.platform != "win32":
            targets.append(self.case / "control\tchar")
        for path in targets:
            with self.subTest(path=path), self.assertRaises(ValueError):
                self.install(base_root=path)
        self.assertFalse((self.game_root / "Inside").exists())
        self.assertEqual(os.listdir(self.win64), [core.TARGET_SHIPPING_EXE])

    def test_corrupt_ledger_never_overwritten_by_install_or_uninstall(self) -> None:
        self.journal.parent.mkdir(parents=True)
        for data in (b"", b'{"state":"installed"', b'[]', b'{"state":"uninstalled"}'):
            self.journal.write_bytes(data)
            self.assertEqual(self.state(), "conflict")
            with self.assertRaisesRegex(RuntimeError, "账本损坏"):
                self.install()
            with self.assertRaisesRegex(RuntimeError, "账本损坏"):
                self.uninstall()
            self.assertEqual(self.journal.read_bytes(), data)

    def test_uninstall_rejects_forged_win64_path_and_ignores_legacy_extras(self) -> None:
        ledger = self.install()
        foreign = self.case / "foreign.dll"
        foreign.write_bytes(b"preserve")
        record = ledger["win64_files"][0]
        original = record.copy()
        record.update(path=str(foreign), sha256=core.sha256_file(foreign))
        self.journal.write_text(json.dumps(ledger), encoding="utf-8")
        with self.assertRaisesRegex(RuntimeError, "白名单"):
            self.uninstall()
        record.update(original)
        ledger["extra_managed_files"] = [{"path": str(foreign), "sha256": core.sha256_file(foreign)}]
        self.journal.write_text(json.dumps(ledger), encoding="utf-8")
        self.assertTrue(self.uninstall()["has_legacy_settings"])
        self.assertEqual(foreign.read_bytes(), b"preserve")

    def test_wal_recovers_interrupted_install_with_pending_file(self) -> None:
        write = core._write_new_file

        def fail_second_bootstrap(path, data):
            if path.name.endswith("-ReShade.ini.tmp"):
                state = json.loads(self.journal.read_bytes())["state"]
                self.assertEqual(state, "installing")
                self.assertTrue((self.win64 / "dxgi.dll").is_file())
                write(path, data[:3])
                raise OSError("injected install failure")
            return write(path, data)

        with patch.object(core, "_write_new_file", side_effect=fail_second_bootstrap):
            with self.assertRaisesRegex(OSError, "injected install failure"):
                self.install()
        self.assertEqual(self.state(), "installing")
        self.assertEqual(self.state(make_game(self.case / "GameB")), "active_elsewhere")
        self.assertEqual(self.install()["state"], "installed")
        self.assertCountEqual(os.listdir(self.win64), [core.TARGET_SHIPPING_EXE, *core.BOOTSTRAP_FILES])

    def test_partial_staging_preserves_manifest_and_can_retry(self) -> None:
        self.install()
        original_move = shutil.move

        def fail_second_move(src, dst):
            manifest = Path(dst).parent / "manifest.json"
            self.assertEqual(len(json.loads(manifest.read_bytes())["entries"]), 2)
            if Path(src).name == "ReShade.ini":
                raise OSError("injected move failure")
            return original_move(src, dst)

        with patch.object(shutil, "move", side_effect=fail_second_move):
            with self.assertRaisesRegex(RuntimeError, "暂存未完成"):
                self.uninstall()
        self.assertEqual(json.loads(self.journal.read_bytes())["state"], "installed")
        self.assertEqual(self.state(), "drifted")
        self.assertEqual(self.uninstall()["state"], "uninstalled")

    def test_engine_ini_unicode_bom_and_crlf(self) -> None:
        self.install()
        self.assert_ini_path((self.base / "wwmi_dx12.ini").read_bytes(),
                             core.format_engine_path(self.wwmi, "windows" if sys.platform == "win32" else "linux-proton"))

        deploy = load_script("stage5_deploy", "scripts/runtime/wwmi-stage5-native-host-deploy.py")
        other = self.case / "legacy-stage5.ini"
        expect = r"Z:\中文模组\测试"
        deploy.write_text_exclusive(other, deploy.candidate_ini(expect), utf16le=True)
        self.assert_ini_path(other.read_bytes(), expect)

    def assert_ini_path(self, data: bytes, expect: str) -> None:
        self.assertTrue(data.startswith(b"\xff\xfe"))
        text = data[2:].decode("utf-16-le")
        self.assertIn("\r\n", text)
        self.assertNotIn("\n", text.replace("\r\n", ""))
        ini = configparser.ConfigParser(interpolation=None)
        ini.read_string(text)
        self.assertEqual(ini["wwmi-dx12"]["wwmi_dir"], expect)

    def test_gui_options_and_no_early_skeleton(self) -> None:
        setup = load_script("linux_setup", "scripts/wwmi-dx12-setup.py")
        parser = setup.build_parser()
        defaults = parser.parse_args(["gui"])
        self.assertEqual(defaults.skin_mode, "default")
        self.assertFalse(defaults.proton_utf8_guard)
        opts = parser.parse_args(["gui", "--skin-mode", "exact", "--proton-utf8-guard", "--dry-run"])
        self.assertEqual(opts.skin_mode, "exact")
        self.assertTrue(opts.proton_utf8_guard and opts.dry_run)

        (self.win64 / "dxgi.dll").write_bytes(b"foreign")
        with patch.object(setup, "resolve_defaults", return_value=(self.win64, None, self.base, None)), \
                patch.object(core, "running_watched_processes", return_value=[]):
            with self.assertRaisesRegex(RuntimeError, "活动账本"):
                setup.gui_install(defaults, self.base)
        self.assertFalse(self.base.exists())

        self.stage("dxgi.dll")
        with patch.object(setup, "resolve_defaults", return_value=(self.win64, None, self.base, None)), \
                patch.object(setup, "REPO_ROOT", self.source), \
                patch.object(core, "running_watched_processes", return_value=[]):
            rc, message = setup.gui_install(defaults, self.base)
        self.assertEqual(rc, 0, message)
        self.assertTrue((self.base / "WWMI/d3dx.ini").is_file())
        self.assertEqual(self.state(), "installed")

    def test_proton_locale_checks_content_and_warns_about_preload_separators(self) -> None:
        compat = self.case / "compatdata/3513350"
        compat.mkdir(parents=True)
        (self.wwmi / "Mods/模组").mkdir()
        base = self.case / "with space"

        settings = compat / "user_settings.py"
        settings.write_text('# user_settings = {"LC_CTYPE":"C.UTF-8"}\nuser_settings = {}\n', encoding="utf-8")
        report = core.run_doctor(self.win64, self.wwmi, base, "linux-proton", compat)
        findings = {f.code: f for f in report.findings}
        self.assertNotIn("检测到 UTF-8 locale", findings["linux_utf8_notice"].message)
        self.assertIn("HOST_LC_ALL=C.UTF-8", findings["linux_utf8_notice"].message)
        self.assertEqual(findings["linux_preload_path"].level, "warn")

        settings.write_text('user_settings = {"LC_CTYPE": "C.UTF-8"}\n', encoding="utf-8")
        report = core.run_doctor(self.win64, self.wwmi, base, "linux-proton", compat)
        legacy = next(f for f in report.findings if f.code == "linux_utf8_notice")
        self.assertIn("检测到 UTF-8 locale", legacy.message)
        self.assertEqual(legacy.level, "warn")

        command, warning = core.proton_launch_options(base, utf8_guard=True)
        self.assertNotIn("LD_PRELOAD=", command)
        self.assertIsNotNone(warning)
        if sys.platform != "win32":
            result = self.install(platform_kind="linux-proton", proton_utf8_guard=True, compatdata_dir=compat)
            self.assertIn("LD_PRELOAD=", result["proton_launch_options"])
            _, warning = core.proton_launch_options(self.case / "with:colon", utf8_guard=True)
            self.assertIsNotNone(warning)
        self.assertEqual(settings.read_text(encoding="utf-8"), 'user_settings = {"LC_CTYPE": "C.UTF-8"}\n')

    def test_flatpak_wwmi_sandbox_isolation_and_tar_ownership_normalization(self) -> None:
        import tarfile

        fake_home = self.case / "HomeFP"
        host_wwmi = core.init_wwmi_skeleton(fake_home / ".local/share/XXMI-Launcher/WWMI")
        fp_data = fake_home / ".var/app/com.valvesoftware.Steam/data"
        fp_data.mkdir(parents=True)
        inst = core.GameInstall(
            label="Steam (Flatpak)",
            platform_kind="linux-proton",
            game_root=self.game_root,
            win64_dir=self.win64,
            compatdata_dir=self.case / "compat-fp",
            is_flatpak_steam=True,
        )
        # Flatpak auto-discovery must not pick unreadable host ~/.local/share/XXMI-Launcher/WWMI
        self.assertEqual(core.discover_wwmi_dirs(home=fake_home, install=inst), [])
        fp_wwmi = core.init_wwmi_skeleton(fp_data / "wwmi-dx12-rt/WWMI")
        self.assertEqual(core.discover_wwmi_dirs(home=fake_home, install=inst), [fp_wwmi])

        # Doctor warns clearly if Flatpak user manually binds a host WWMI path outside sandbox
        rep = core.run_doctor(self.win64, host_wwmi, fp_data / "wwmi-dx12-rt", "linux-proton", inst.compatdata_dir, True)
        self.assertIn("flatpak_wwmi_sandbox", {f.code for f in rep.findings})

        # Linux uninstall noop must report actual noop text (never claim success)
        setup = load_script("wwmi_dx12_setup_noop", "scripts/wwmi-dx12-setup.py")
        args = type("Args", (), {"base_root": str(self.base), "dry_run": False})()
        rc, msg = setup._run_uninstall(args)
        self.assertEqual(rc, 0)
        self.assertIn("未发现活动中的安装账本", msg)

        # package_release.make_targz normalizes tar member uid/gid/uname/gname to 0/root
        pkg = load_script("package_release_test", "scripts/package_release.py")
        sample_dir = self.case / "WWMI-DX12-RT-linux"
        sample_dir.mkdir()
        (sample_dir / "wwmi-dx12-setup").write_text("#!/bin/sh\n", encoding="utf-8")
        out_tar = self.case / "out.tar.gz"
        pkg.make_targz(sample_dir, out_tar)
        with tarfile.open(out_tar, "r:gz") as tf:
            for member in tf.getmembers():
                self.assertEqual((member.uid, member.gid, member.uname, member.gname), (0, 0, "root", "root"))


if __name__ == "__main__":
    unittest.main()
