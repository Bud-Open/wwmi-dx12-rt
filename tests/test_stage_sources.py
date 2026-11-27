"""Regression checks against the real stage_sources merger, using tiny upstream fixtures."""
import importlib.util
import json
import os
import re
import tempfile
import time
import unittest
from pathlib import Path
from unittest.mock import patch

REPO_ROOT = Path(__file__).resolve().parent.parent
SCRIPT = REPO_ROOT / "cmake/stage_sources.py"
SPEC = importlib.util.spec_from_file_location("stage_sources", SCRIPT)
assert SPEC and SPEC.loader
stage_sources = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(stage_sources)


class TestStageSources(unittest.TestCase):
    def test_rename_and_removal_archive_stale_files_and_refresh_mtime(self) -> None:
        root = Path(tempfile.mkdtemp(prefix="wwmi-stage-sources-")).resolve()
        r, up, staged = root / "repo", root / "upstream", root / "staged"
        header = up / "reshade/include/reshade.hpp"
        header.parent.mkdir(parents=True)
        header.touch()
        (up / "wwmi-dx12/reshade_addon").mkdir(parents=True)
        legacy = up / "wwmi-dx12/third_party/3Dmigoto/DirectX11"
        legacy.mkdir(parents=True)
        (legacy / "CommandList.cpp").touch()
        with self.assertRaisesRegex(FileNotFoundError, r"Missing upstream 3Dmigoto.*bootstrap_deps\.py"):
            stage_sources.stage_all(r, up, staged)

        migoto = up / "3Dmigoto/DirectX11"
        migoto.mkdir(parents=True)
        names = re.search(r"tdm_subset = \((.*?)\n    \)", SCRIPT.read_text(encoding="utf-8"), re.S)
        self.assertIsNotNone(names)
        for name in re.findall(r'"([^"]+)"', names[1]):
            (migoto / name).touch()
        (migoto / "CommandList.cpp").write_text("standalone", encoding="utf-8")

        overlay = r / "source/host"
        overlay.mkdir(parents=True)
        for name in ("keep.cpp", "old.cpp", "removed.cpp"):
            (overlay / name).write_text("old", encoding="utf-8")
        fake_home = root / "home"
        with patch.object(Path, "home", return_value=fake_home):
            stage_sources.stage_all(r, up, staged)
            self.assertEqual((staged / "3dm_src/CommandList.cpp").read_text(encoding="utf-8"), "standalone")
            live = staged / "reshade"
            keep = live / "keep.cpp"
            before = keep.stat().st_mtime_ns
            stage_sources.stage_all(r, up, staged)
            self.assertEqual(keep.stat().st_mtime_ns, before)

            (overlay / "old.cpp").rename(overlay / "renamed.cpp")
            stage_sources.Archive("fixture-removal").move(overlay / "removed.cpp", Path("removed.cpp"))
            # Ensure distinct mtimes on Windows even when consecutive passes fit within one clock tick.
            time.sleep(0.03)
            (overlay / "keep.cpp").write_text("new", encoding="utf-8")
            older_ns = before - 4_000_000_000
            os.utime(overlay / "keep.cpp", ns=(older_ns, older_ns))
            stage_sources.stage_all(r, up, staged)

        self.assertFalse((live / "old.cpp").exists())
        self.assertFalse((live / "removed.cpp").exists())
        self.assertEqual((live / "renamed.cpp").read_text(encoding="utf-8"), "old")
        self.assertEqual(keep.read_text(encoding="utf-8"), "new")
        self.assertGreater(keep.stat().st_mtime_ns, before)
        (manifest,) = (fake_home / ".pi/deletion-staging").glob("batch-*-wwmi-stale-sources*/manifest.json")
        entries = json.loads(manifest.read_bytes())
        self.assertCountEqual([Path(e["original"]).name for e in entries], ["old.cpp", "removed.cpp"])
        self.assertTrue(all(Path(e["staged"]).read_bytes() == b"old" for e in entries))
