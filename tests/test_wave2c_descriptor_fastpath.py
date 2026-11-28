import pathlib
import subprocess
import tempfile
import unittest


REPO_ROOT = pathlib.Path(__file__).resolve().parent.parent
TEST_CPP = REPO_ROOT / "tests" / "test_wave2c_descriptor_fastpath.cpp"


class Wave2CDescriptorFastPathTests(unittest.TestCase):
    def test_cpp_concurrency_and_mask_invariants(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wwmi_w2c_test_") as td:
            exe = pathlib.Path(td) / "test_wave2c_descriptor_fastpath"
            comp = subprocess.run(
                ["clang++", "-std=c++20", "-O2", "-pthread", str(TEST_CPP), "-o", str(exe)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(comp.returncode, 0, msg=f"compile failed:\n{comp.stderr}")
            run = subprocess.run([str(exe)], capture_output=True, text=True, check=False, timeout=30)
            self.assertEqual(run.returncode, 0, msg=f"test failed:\n{run.stdout}\n{run.stderr}")
            self.assertIn("WAVE2C_DESCRIPTOR_FASTPATH_TESTS_PASS", run.stdout)


if __name__ == "__main__":
    unittest.main()
