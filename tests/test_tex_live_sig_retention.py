import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class TexLiveSigRetentionTest(unittest.TestCase):
    def test_resident_first_eviction_and_refcounts(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "test_tex_live_sig_retention"
            subprocess.run(
                [
                    "g++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "source/main/reshade_addon/textures"),
                    str(ROOT / "tests/test_tex_live_sig_retention.cpp"),
                    "-o",
                    str(exe),
                ],
                check=True,
            )
            out = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
            self.assertIn("PASS", out.stdout)


if __name__ == "__main__":
    unittest.main()
