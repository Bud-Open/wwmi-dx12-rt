"""Compile and test the host WWMI_BindingTrace implementation and header sync."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
COMPILER = shutil.which("c++") or shutil.which("clang++")
HOST_D3D12 = ROOT / "source/host/source/d3d12"


class BindingTraceHeaderSyncTests(unittest.TestCase):
    def test_abi_headers_in_sync(self):
        headers = [
            ROOT / "source/include/wwmi_binding_trace.h",
            ROOT / "source/host/source/d3d12/wwmi_binding_trace.h",
            ROOT / "source/main/reshade_addon/observation/binding_bridge/wwmi_binding_trace.h",
        ]
        ref = headers[0].read_bytes()
        for h in headers[1:]:
            self.assertEqual(h.read_bytes(), ref, f"Header drift detected: {h}")

        helpers = [
            ROOT / "source/include/wwmi_binding_trace_d3d12.hpp",
            ROOT / "source/host/source/d3d12/wwmi_binding_trace_d3d12.hpp",
        ]
        self.assertEqual(helpers[1].read_bytes(), helpers[0].read_bytes(),
                         "wwmi_binding_trace_d3d12.hpp copies diverged")


@unittest.skipUnless(COMPILER, "requires a host C++ compiler")
class BindingTraceRuntimeTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        scratch = tempfile.TemporaryDirectory()
        cls.addClassCleanup(scratch.cleanup)
        cls.binary = Path(scratch.name) / "binding_trace_tests"
        subprocess.run([
            COMPILER, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror", "-pthread",
            "-I", str(HOST_D3D12),
            str(ROOT / "tests/test_binding_trace.cpp"),
            str(HOST_D3D12 / "wwmi_binding_trace.cpp"),
            "-o", str(cls.binary),
        ], check=True, timeout=25)

    def test_legacy_register_receives_draws(self):
        subprocess.run([str(self.binary), "legacy"], check=True, timeout=5)

    def test_register_ex_skip_draws(self):
        subprocess.run([str(self.binary), "skip_draws"], check=True, timeout=5)
