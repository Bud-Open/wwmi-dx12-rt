import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent

DXGIFORMAT_STUB = """#pragma once
enum DXGI_FORMAT {
    DXGI_FORMAT_UNKNOWN = 0,
    DXGI_FORMAT_R8G8B8A8_UNORM = 28,
    DXGI_FORMAT_R8G8B8A8_SNORM = 31,
    DXGI_FORMAT_R16G16_FLOAT = 34,
};
"""


class NativeDiscoveryWorkMaskTest(unittest.TestCase):
    def test_settled_rest_proof_and_disabled_intent_filter(self) -> None:
        src = ROOT / "tests/test_native_discovery_work_mask.cpp"
        rt_pack = ROOT / "source/client/rt_pack.cpp"
        with tempfile.TemporaryDirectory() as tmp:
            tmp_path = Path(tmp)
            stub_inc = tmp_path / "stub_inc"
            stub_inc.mkdir()
            (stub_inc / "dxgiformat.h").write_text(DXGIFORMAT_STUB, encoding="utf-8")
            exe = tmp_path / "test_native_discovery_work_mask"
            cmd = [
                "g++",
                "-std=c++20",
                "-O2",
                "-Wall",
                "-Wextra",
                f"-I{stub_inc}",
                f"-I{ROOT / 'source/client'}",
                f"-I{ROOT / 'source/include'}",
                str(src),
                str(rt_pack),
                "-o",
                str(exe),
            ]
            res = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(res.returncode, 0, f"Compile failed:\n{res.stderr}")
            run = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(run.returncode, 0, f"Run failed:\n{run.stdout}\n{run.stderr}")
            self.assertIn("OK: test_native_discovery_work_mask", run.stdout)


if __name__ == "__main__":
    unittest.main()
