"""Compile and run legacy texture upload layout + host footprint bridge unit tests."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
COMPILER = shutil.which("c++") or shutil.which("clang++")
UPSTREAM_DXGI = ROOT / "third_party/wwmi-dx12/src/dxgi_bc.h"


@unittest.skipUnless(COMPILER and UPSTREAM_DXGI.is_file(), "requires a host C++ compiler and bootstrapped dxgi_bc.h")
class TexUploadLayoutTests(unittest.TestCase):
    def test_upload_layout_and_host_footprint_bridge(self) -> None:
        src = ROOT / "tests/test_tex_upload_layout.cpp"
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "test_tex_upload_layout"
            cmd = [
                COMPILER,
                "-std=c++20",
                "-O2",
                "-Wall",
                "-Wextra",
                "-Werror",
                f"-I{ROOT / 'source/main/reshade_addon/textures'}",
                f"-I{ROOT / 'third_party/wwmi-dx12/reshade_addon/textures'}",
                f"-I{ROOT / 'source/include'}",
                f"-I{ROOT / 'source/host/source/d3d12'}",
                str(src),
                "-o",
                str(exe),
            ]
            res = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(res.returncode, 0, f"Compile failed:\n{res.stderr}")
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, f"Run failed:\n{run.stdout}\n{run.stderr}")
            self.assertIn("OK: test_tex_upload_layout", run.stdout)


if __name__ == "__main__":
    unittest.main()
