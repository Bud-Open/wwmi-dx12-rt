#!/usr/bin/env python3
"""Unit tests for tools/win_setup/mini_unzip.hpp (RFC 1951 Inflate + ZIP security guards)."""
from __future__ import annotations

import os
import stat
import subprocess
import tempfile
import unittest
import zipfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent


class MiniUnzipTests(unittest.TestCase):
    _bin_dir: tempfile.TemporaryDirectory[str] | None = None
    _exe: Path | None = None

    @classmethod
    def setUpClass(cls) -> None:
        cls._bin_dir = tempfile.TemporaryDirectory(prefix="wwmi-mini-unzip-test-")
        cls._exe = Path(cls._bin_dir.name) / "test_mini_unzip"
        compiler = "g++"
        cmd = [
            compiler,
            "-std=c++17",
            "-O2",
            "-Wall",
            "-Wextra",
            "-Werror",
            f"-I{ROOT / 'tools/win_setup'}",
            str(ROOT / "tests/test_mini_unzip.cpp"),
            "-o",
            str(cls._exe),
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            raise RuntimeError(f"Failed compiling test_mini_unzip.cpp:\n{res.stdout}\n{res.stderr}")

    @classmethod
    def tearDownClass(cls) -> None:
        if cls._bin_dir is not None:
            cls._bin_dir.cleanup()

    def run_harness(self, zip_path: Path, req_root: str = "-", max_unpacked: int | None = None) -> subprocess.CompletedProcess[str]:
        assert self._exe is not None
        args = [str(self._exe), str(zip_path), req_root]
        if max_unpacked is not None:
            args.append(str(max_unpacked))
        return subprocess.run(args, capture_output=True, text=True)

    def test_deflate_and_stored_roundtrip_exact_crc(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wwmi-zip-ok-") as tmp:
            zpath = Path(tmp) / "good.zip"
            payloads = {
                "empty.bin": b"",
                "small.txt": b"WWMI-DX12-RT hello world!\n",
                "stored.dat": os.urandom(4096),
                "compressible.txt": (b"ABCD1234" * 8192) + b"TAIL_MARKER",
                "mixed/deep/random.bin": os.urandom(131072) + (b"\x5a" * 65536) + os.urandom(32768),
            }
            with zipfile.ZipFile(zpath, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=6) as zf:
                for name, data in payloads.items():
                    comp = zipfile.ZIP_STORED if name == "stored.dat" else zipfile.ZIP_DEFLATED
                    zf.writestr(f"WWMI-DX12-RT-win64/{name}", data, compress_type=comp)

            res = self.run_harness(zpath, "WWMI-DX12-RT-win64")
            self.assertEqual(res.returncode, 0, res.stderr)
            seen: dict[str, tuple[int, int]] = {}
            for line in res.stdout.splitlines():
                if line.startswith("FILE "):
                    _, rel, sz_s, crc_s = line.split()
                    seen[rel] = (int(sz_s), int(crc_s, 16))
            expected = {k: (len(v), zlib.crc32(v) & 0xFFFFFFFF) for k, v in payloads.items()}
            self.assertEqual(seen, expected)

    def test_rejects_zip_slip_and_escape_root(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wwmi-zip-slip-") as tmp:
            for bad_name in (
                "WWMI-DX12-RT-win64/../evil.dll",
                "../evil.dll",
                "/abs/evil.dll",
                "WWMI-DX12-RT-win64/sub\\evil.dll",
                "OtherRoot/ok.txt",
                "WWMI-DX12-RT-win64/CON.txt",
                "WWMI-DX12-RT-win64/foo:ads.txt",
                "WWMI-DX12-RT-win64/trailing.",
            ):
                zpath = Path(tmp) / "bad.zip"
                with zipfile.ZipFile(zpath, "w", compression=zipfile.ZIP_DEFLATED) as zf:
                    zf.writestr(bad_name, b"nope")
                res = self.run_harness(zpath, "WWMI-DX12-RT-win64")
                self.assertNotEqual(res.returncode, 0, f"Expected reject for {bad_name}, got OK")

    def test_rejects_case_collision_symlink_crc_tamper_and_bomb(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wwmi-zip-adv-") as tmp:
            # 1. Case-insensitive collision
            z_case = Path(tmp) / "case.zip"
            with zipfile.ZipFile(z_case, "w", compression=zipfile.ZIP_DEFLATED) as zf:
                zf.writestr("Root/File.dll", b"aaa")
                zf.writestr("Root/file.dll", b"bbb")
            self.assertNotEqual(self.run_harness(z_case, "Root").returncode, 0)

            # 2. Unix symlink entry
            z_sym = Path(tmp) / "sym.zip"
            with zipfile.ZipFile(z_sym, "w") as zf:
                zi = zipfile.ZipInfo("Root/link.dll")
                zi.create_system = 3  # Unix
                zi.external_attr = (stat.S_IFLNK | 0o777) << 16
                zf.writestr(zi, b"/etc/passwd")
            self.assertNotEqual(self.run_harness(z_sym, "Root").returncode, 0)

            # 3. Decompression bomb exceeding cap
            z_bomb = Path(tmp) / "bomb.zip"
            with zipfile.ZipFile(z_bomb, "w", compression=zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
                zf.writestr("Root/huge.bin", b"\x00" * (2 * 1024 * 1024))
            res_bomb = self.run_harness(z_bomb, "Root", max_unpacked=1024 * 1024)
            self.assertNotEqual(res_bomb.returncode, 0)

            # 4. CRC-32 corruption
            z_crc = Path(tmp) / "crc.zip"
            with zipfile.ZipFile(z_crc, "w", compression=zipfile.ZIP_DEFLATED) as zf:
                zf.writestr("Root/data.txt", b"integrity-check-payload" * 100)
            raw = bytearray(z_crc.read_bytes())
            # Flip CRC in Central Directory header (signature PK\x01\x02, crc at +16)
            cd_idx = raw.index(b"PK\x01\x02")
            raw[cd_idx + 16] ^= 0xFF
            z_crc.write_bytes(bytes(raw))
            res_crc = self.run_harness(z_crc, "Root")
            self.assertNotEqual(res_crc.returncode, 0)
            self.assertIn("CRC-32", res_crc.stderr)


if __name__ == "__main__":
    unittest.main()
