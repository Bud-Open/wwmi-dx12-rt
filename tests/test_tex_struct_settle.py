import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class TexStructSettleTest(unittest.TestCase):
    def test_missing_signature_requeue_and_settle(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            exe = Path(tmp) / "test_tex_struct_settle"
            subprocess.run(
                [
                    "g++",
                    "-std=c++20",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-I",
                    str(ROOT / "source/main/reshade_addon/textures"),
                    str(ROOT / "tests/test_tex_struct_settle.cpp"),
                    "-o",
                    str(exe),
                ],
                check=True,
            )
            out = subprocess.run([str(exe)], check=True, capture_output=True, text=True)
            self.assertIn("PASS", out.stdout)


if __name__ == "__main__":
    unittest.main()
