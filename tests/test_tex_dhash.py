"""Compile and exercise the production CPU dHash with the real bcdec on the host."""

import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
COMPILER = shutil.which("c++") or shutil.which("clang++")
TEXTURES = ROOT / "source/main/reshade_addon/textures"
UPSTREAM = ROOT / "third_party/wwmi-dx12"
BCDEC = UPSTREAM / "third_party/bcdec/bcdec.h"


@unittest.skipUnless(COMPILER and BCDEC.is_file(), "requires a host C++ compiler and bootstrapped bcdec.h")
class TexDHashTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        scratch = tempfile.TemporaryDirectory()
        cls.addClassCleanup(scratch.cleanup)
        cls.binary = Path(scratch.name) / "tex_dhash_tests"
        subprocess.run([
            COMPILER, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
            "-I", str(TEXTURES),
            # Resolve the same relative bcdec include as in the CMake staged tree.
            "-I", str(UPSTREAM / "reshade_addon/textures"),
            str(ROOT / "tests/test_tex_dhash.cpp"), str(TEXTURES / "tex_dhash.cpp"),
            "-o", str(cls.binary),
        ], check=True, timeout=25)

    def run_case(self, name):
        subprocess.run([str(self.binary), name], check=True, timeout=5)

    def test_horizontal_gradient_and_bit_order(self):
        self.run_case("gradient")

    def test_resolution_invariance(self):
        self.run_case("resolution")

    def test_thermometer_fallback_and_zero_hash(self):
        self.run_case("solid")

    def test_srgb_view_and_alpha(self):
        self.run_case("srgb")

    def test_bc1_and_bc7_mode_6(self):
        self.run_case("blocks")

    def test_sparse_rows_cover_every_fetch(self):
        self.run_case("rows")

    def test_layout_for_all_supported_formats(self):
        self.run_case("layout")

    def test_padded_upload_footprint_and_src_box(self):
        self.run_case("upload_footprint")
