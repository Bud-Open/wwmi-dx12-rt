import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


class NativeDiscoveryWorkMaskTest(unittest.TestCase):
    def test_settled_rest_proof_and_disabled_intent_filter(self) -> None:
        src = ROOT / "tests/test_native_discovery_work_mask.cpp"
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "test_native_discovery_work_mask"
            cmd = [
                "g++",
                "-std=c++20",
                "-O2",
                "-Wall",
                "-Wextra",
                f"-I{ROOT / 'source/client'}",
                f"-I{ROOT / 'source/include'}",
                str(src),
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
