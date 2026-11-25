"""Compile the production worker solver with the host compiler and exercise it directly."""
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(shutil.which("g++"), "requires host g++")
class TexStructSolverTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        scratch = tempfile.TemporaryDirectory()
        cls.addClassCleanup(scratch.cleanup)
        cls.binary = str(Path(scratch.name) / "tex_struct_solver_tests")
        subprocess.run([
            "g++", "-std=c++20", "-Wall", "-Wextra", "-Werror",
            "-I", str(ROOT / "source/main/reshade_addon/textures"),
            str(ROOT / "tests/test_tex_struct_solver.cpp"), "-o", cls.binary,
        ], check=True, timeout=20)

    def test_reclaim_twin_before_reuse(self):
        subprocess.run([self.binary, "reclaim"], check=True, timeout=5)

    def test_resolution_precedes_weak_histogram_match(self):
        subprocess.run([self.binary, "resolution"], check=True, timeout=5)

    def test_protect_high_ssim(self):
        subprocess.run([self.binary, "direct"], check=True, timeout=5)

    def test_only_differing_patch_separates_weak_twins(self):
        subprocess.run([self.binary, "patch"], check=True, timeout=5)

    def test_non_twin_and_cooccurrence_aware_reuse(self):
        subprocess.run([self.binary, "reuse"], check=True, timeout=5)

    def test_prune_entire_transitive_cluster(self):
        subprocess.run([self.binary, "cluster"], check=True, timeout=5)

    def test_cutout_mask_and_white_neutralizer_classifier(self):
        subprocess.run([self.binary, "cutout"], check=True, timeout=5)

    def test_slot_shader_key_separates_ps_decl_masks(self):
        subprocess.run([self.binary, "slotkey"], check=True, timeout=5)
