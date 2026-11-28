#!/usr/bin/env python3
"""Tests for update-manifest.json generation and archive parity in scripts/package_release.py."""
from __future__ import annotations

import hashlib
import importlib.util
import json
import sys
import tarfile
import tempfile
import unittest
import zipfile
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
if str(ROOT / "scripts") not in sys.path:
    sys.path.insert(0, str(ROOT / "scripts"))


def load_package_release():
    spec = importlib.util.spec_from_file_location("package_release_manifest_test", ROOT / "scripts/package_release.py")
    assert spec and spec.loader
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class ReleaseManifestTests(unittest.TestCase):
    def test_win64_and_linux_manifest_entries_match_archives(self) -> None:
        pkg = load_package_release()
        with tempfile.TemporaryDirectory(prefix="wwmi-manifest-test-") as tmp:
            base = Path(tmp)
            win_dir = base / "WWMI-DX12-RT-win64"
            (win_dir / "host").mkdir(parents=True)
            (win_dir / "reshade-addons/wwmi_dx12").mkdir(parents=True)
            (win_dir / "BUILD-PROVENANCE.json").write_text('{"version":"v1.0.5"}\n', encoding="utf-8")
            (win_dir / "host/ReShade64-game-forward.dll").write_bytes(b"WIN_HOST_DLL" * 128)
            (win_dir / "reshade-addons/wwmi_dx12/Bc7Half.cso").write_bytes(b"DXBC" + b"\x11" * 256)
            win_zip = base / "WWMI-DX12-RT-win64.zip"
            pkg.make_zip(win_dir, win_zip)

            lin_dir = base / "WWMI-DX12-RT-linux"
            (lin_dir / "host").mkdir(parents=True)
            (lin_dir / "lib").mkdir(parents=True)
            (lin_dir / "wwmi-dx12-setup").write_text("#!/bin/sh\necho hi\n", encoding="utf-8")
            (lin_dir / "wwmi-dx12-setup").chmod(0o755)
            (lin_dir / "host/ReShade64-game-forward.dll").write_bytes(b"LIN_HOST_DLL" * 64)
            (lin_dir / "lib/wine-filename-utf8.so").write_bytes(b"\x7fELF" + b"\x00" * 64)
            lin_tar = base / "WWMI-DX12-RT-linux.tar.gz"
            pkg.make_targz(lin_dir, lin_tar)

            assets = {
                "win-x64": pkg.build_asset_manifest_entry(win_dir, win_zip, "zip"),
                "linux-x64": pkg.build_asset_manifest_entry(lin_dir, lin_tar, "tar.gz", pkg.LINUX_EXECUTABLES),
            }
            manifest = pkg.build_update_manifest(
                {"version": "v1.0.5", "repository_commit": "a" * 40},
                assets,
                published_at="2026-11-28T10:00:00Z",
            )
            out_path = pkg.write_update_manifest(manifest, base / "update-manifest.json")
            parsed = json.loads(out_path.read_text(encoding="utf-8"))

            self.assertEqual(parsed["schema_version"], 1)
            self.assertEqual(parsed["min_updater_protocol"], 1)
            self.assertEqual(parsed["repo"], "Bud-Open/wwmi-dx12-rt")
            self.assertEqual(parsed["version"], "v1.0.5")
            self.assertEqual(parsed["release_url"], "https://github.com/Bud-Open/wwmi-dx12-rt/releases/tag/v1.0.5")

            # Verify zip member parity
            w = parsed["assets"]["win-x64"]
            self.assertEqual(w["name"], "WWMI-DX12-RT-win64.zip")
            self.assertEqual(w["bytes"], win_zip.stat().st_size)
            self.assertEqual(w["sha256"], hashlib.sha256(win_zip.read_bytes()).hexdigest())
            with zipfile.ZipFile(win_zip) as zf:
                zip_map = {
                    info.filename[len(w["root"]) + 1:]: hashlib.sha256(zf.read(info)).hexdigest()
                    for info in zf.infolist() if not info.is_dir()
                }
            manifest_win_map = {f["path"]: f["sha256"] for f in w["files"]}
            self.assertEqual(manifest_win_map, zip_map)
            self.assertEqual(w["file_count"], len(zip_map))

            # Verify tar.gz member parity and executable flag
            l = parsed["assets"]["linux-x64"]
            self.assertEqual(l["name"], "WWMI-DX12-RT-linux.tar.gz")
            self.assertEqual(l["bytes"], lin_tar.stat().st_size)
            self.assertEqual(l["sha256"], hashlib.sha256(lin_tar.read_bytes()).hexdigest())
            tar_map: dict[str, str] = {}
            with tarfile.open(lin_tar, "r:gz") as tf:
                for m in tf.getmembers():
                    if m.isfile():
                        fobj = tf.extractfile(m)
                        assert fobj is not None
                        tar_map[m.name[len(l["root"]) + 1:]] = hashlib.sha256(fobj.read()).hexdigest()
            manifest_lin_map = {f["path"]: f["sha256"] for f in l["files"]}
            self.assertEqual(manifest_lin_map, tar_map)
            exec_entries = {f["path"] for f in l["files"] if f.get("executable")}
            self.assertEqual(exec_entries, {"wwmi-dx12-setup"})


if __name__ == "__main__":
    unittest.main()
