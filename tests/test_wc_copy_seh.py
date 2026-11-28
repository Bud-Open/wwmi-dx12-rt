"""Compile and run real OS page-fault SEH tests for wc_copy_seh.hpp (/O2 /EHsc -msse4.1)."""

import os
import platform
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
WRAP_CLANG_CL = ROOT / "build/win64/_toolchain_wrappers/clang-cl"
WINE = shutil.which("wine")
LLVM_READOBJ = shutil.which("llvm-readobj")
LLVM_OBJDUMP = shutil.which("llvm-objdump")
IS_WINDOWS = platform.system() == "Windows"


def _find_xwin_root():
    for candidate in (
        os.environ.get("XWIN_ROOT"),
        str(Path.home() / ".local/share/wwmi-build-deps/xwin-sdk"),
        str(Path.home() / ".xwin"),
    ):
        if candidate and (Path(candidate) / "crt/include").exists():
            return Path(candidate)
    return None


def _find_lld_link():
    for candidate in (
        os.environ.get("LLVM_MINGW_ROOT") and str(Path(os.environ["LLVM_MINGW_ROOT"]) / "bin/lld-link"),
        str(Path.home() / ".local/share/wwmi-build-deps/llvm-mingw/bin/lld-link"),
        shutil.which("lld-link"),
    ):
        if candidate and Path(candidate).exists():
            return candidate
    return None


def _find_compiler():
    if IS_WINDOWS:
        return shutil.which("cl") or shutil.which("clang-cl")
    if WRAP_CLANG_CL.exists():
        return str(WRAP_CLANG_CL)
    return None


COMPILER = _find_compiler()
XWIN_ROOT = None if IS_WINDOWS else _find_xwin_root()
LLD_LINK = None if IS_WINDOWS else _find_lld_link()
CAN_RUN = bool(COMPILER and (IS_WINDOWS or (WINE and XWIN_ROOT and LLD_LINK)))


@unittest.skipUnless(CAN_RUN, "requires clang-cl/cl (+ xwin/lld-link/wine on Linux)")
class WcCopySehTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        scratch = tempfile.TemporaryDirectory()
        cls.addClassCleanup(scratch.cleanup)
        cls.workdir = Path(scratch.name)
        cls.obj = cls.workdir / "test_wc_copy_seh.obj"
        cls.exe = cls.workdir / "test_wc_copy_seh.exe"
        compile_cmd = [
            COMPILER,
            "/nologo",
            "/c",
            "/O2",
            "/EHsc",
            "/MT",
            "/std:c++20",
            "/D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH",
            f"/I{ROOT / 'source/include'}",
        ]
        if not IS_WINDOWS or "clang-cl" in Path(COMPILER).name.lower():
            compile_cmd += ["-clang:-msse4.1"]
        compile_cmd += [
            f"/Fo{cls.obj}",
            str(ROOT / "tests/test_wc_copy_seh.cpp"),
        ]
        subprocess.run(compile_cmd, check=True, timeout=30)

        if IS_WINDOWS:
            link_cmd = [
                COMPILER,
                "/nologo",
                "/MT",
                str(cls.obj),
                f"/Fe{cls.exe}",
                "/link",
                "kernel32.lib",
            ]
        else:
            link_cmd = [
                LLD_LINK,
                "/nologo",
                "/manifest:no",
                f"/libpath:{XWIN_ROOT / 'crt/lib/x86_64'}",
                f"/libpath:{XWIN_ROOT / 'sdk/lib/ucrt/x86_64'}",
                f"/libpath:{XWIN_ROOT / 'sdk/lib/um/x86_64'}",
                f"/out:{cls.exe}",
                str(cls.obj),
                "kernel32.lib",
            ]
        subprocess.run(link_cmd, check=True, timeout=30)

    def test_seh_unwind_handler_emitted(self):
        if not LLVM_READOBJ:
            self.skipTest("llvm-readobj unavailable")
        res = subprocess.run(
            [LLVM_READOBJ, "--unwind", str(self.obj)],
            check=True,
            capture_output=True,
            text=True,
            timeout=15,
        )
        self.assertIn(
            "__C_specific_handler",
            res.stdout,
            "clang-cl /O2 /EHsc must emit __C_specific_handler for safe_copy_from_wc",
        )
        if LLVM_OBJDUMP:
            dis = subprocess.run(
                [LLVM_OBJDUMP, "-d", str(self.obj)],
                check=True,
                capture_output=True,
                text=True,
                timeout=15,
            )
            self.assertIn("movntdqa", dis.stdout, "copy_from_wc_leaf must emit SSE4.1 movntdqa")
            self.assertGreaterEqual(
                dis.stdout.count("mfence"),
                2,
                "copy_from_wc_leaf must bracket movntdqa streaming loads with mfence",
            )

    def test_alignment_and_real_page_faults(self):
        env = os.environ.copy()
        env["WINEDEBUG"] = "-all"
        cmd = [str(self.exe)] if IS_WINDOWS else [WINE, str(self.exe)]
        res = subprocess.run(cmd, check=True, capture_output=True, text=True, env=env, timeout=20)
        self.assertIn("OK: wc_copy_seh tests passed", res.stdout)
