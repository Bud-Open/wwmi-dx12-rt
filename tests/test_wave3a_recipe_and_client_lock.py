from __future__ import annotations

import pathlib
import subprocess
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
CPP_SRC = REPO_ROOT / "tests" / "test_wave3a_recipe_and_client_lock.cpp"


class Wave3ARecipeAndClientLockTests(unittest.TestCase):
    def test_wave3a_recipe_cache_and_client_lock_stats(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            exe = pathlib.Path(td) / "test_wave3a_recipe_and_client_lock"
            cmd = [
                "g++",
                "-std=c++20",
                "-O2",
                "-Wall",
                "-Wextra",
                "-pthread",
                str(CPP_SRC),
                "-o",
                str(exe),
            ]
            compile_res = subprocess.run(cmd, capture_output=True, text=True)
            self.assertEqual(
                compile_res.returncode,
                0,
                msg=f"g++ failed:\nstdout:\n{compile_res.stdout}\nstderr:\n{compile_res.stderr}",
            )
            run_res = subprocess.run([str(exe)], capture_output=True, text=True)
            self.assertEqual(
                run_res.returncode,
                0,
                msg=f"test binary failed:\nstdout:\n{run_res.stdout}\nstderr:\n{run_res.stderr}",
            )
            self.assertIn("OK: test_wave3a_recipe_and_client_lock", run_res.stdout)


if __name__ == "__main__":
    unittest.main()
