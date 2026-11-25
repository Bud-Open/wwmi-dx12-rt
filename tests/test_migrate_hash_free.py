#!/usr/bin/env python3
"""Automated unit tests for tools/migrate_hash_free.py."""
from __future__ import annotations

import json
import struct
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import migrate_hash_free as mh  # noqa: E402
import texlook_tool as tl  # noqa: E402


def _write_synthetic_dds(path: Path, width: int, height: int, dxgi_fmt: int) -> None:
    """Write a minimal valid 148-byte DX10 DDS header + dummy payload."""
    hdr = bytearray(148)
    hdr[0:4] = b"DDS "
    struct.pack_into("<I", hdr, 4, 124)
    struct.pack_into("<I", hdr, 8, 0x1007)
    struct.pack_into("<I", hdr, 12, height)
    struct.pack_into("<I", hdr, 16, width)
    struct.pack_into("<I", hdr, 28, 1)  # mips
    struct.pack_into("<I", hdr, 76, 32)  # pf size
    struct.pack_into("<I", hdr, 80, 0x4)  # DDPF_FOURCC
    hdr[84:88] = b"DX10"
    struct.pack_into("<I", hdr, 128, dxgi_fmt)
    struct.pack_into("<I", hdr, 132, 3)  # D3D10_RESOURCE_DIMENSION_TEXTURE2D
    struct.pack_into("<I", hdr, 140, 1)  # arraySize
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(bytes(hdr) + b"\x00" * 64)


class TestMigrateHashFree(unittest.TestCase):
    def test_structural_migration_anchors_ambiguity_and_override(self) -> None:
        root = Path(tempfile.mkdtemp(prefix="wwmi-test-"))
        mod_dir = root / "SampleMod"
        tex_dir = mod_dir / "Textures"
        out_dir = root / "out"

        # Component 0: 1 anchor (R8 re-saved as BC7_UNORM!), 1 unique BC7_UNORM, 1 unique BC7_UNORM_SRGB, 1 explicit ps-t4 = null
        # Component 1: 2 BC7_UNORM_SRGB files -> MUST be marked AMBIGUOUS (no blind guessing!)
        _write_synthetic_dds(tex_dir / "Components-0 t=11111111.dds", 1024, 1024, 98)  # anchor R8 re-saved as BC7_UNORM
        _write_synthetic_dds(tex_dir / "Components-0 t=22222222.dds", 1024, 1024, 98)  # true BC7_UNORM
        _write_synthetic_dds(tex_dir / "Components-0 t=33333333.dds", 1024, 1024, 99)  # unique BC7_UNORM_SRGB
        _write_synthetic_dds(tex_dir / "Components-1 t=44444444.dds", 1024, 1024, 99)  # ambiguous #1
        _write_synthetic_dds(tex_dir / "Components-1 t=55555555.dds", 1024, 1024, 99)  # ambiguous #2

        orig_ini = (
            "[Constants]\n"
            "global $object_guid = 54321\n"
            "global $mod_enabled = 0\n"
            "global $object_detected = 0\n\n"
            "[TextureOverrideComponent0]\n"
            "hash = aabbccdd\n"
            "match_first_index = 0\n"
            "match_index_count = 600\n"
            "$object_detected = 1\n"
            "if $mod_enabled\n"
            "    handling = skip\n"
            "    run = CommandListTriggerResourceOverrides\n"
            "    run = CommandListOverrideSharedResources\n"
            "    ps-t4 = null\n"
            "    drawindexed = 600, 0, 0\n"
            "    run = CommandListCleanupSharedResources\n"
            "endif\n\n"
            "[TextureOverrideComponent1]\n"
            "hash = aabbccdd\n"
            "match_first_index = 600\n"
            "match_index_count = 300\n"
            "$object_detected = 1\n"
            "if $mod_enabled\n"
            "    handling = skip\n"
            "    run = CommandListTriggerResourceOverrides\n"
            "    run = CommandListOverrideSharedResources\n"
            "    drawindexed = 300, 600, 0\n"
            "    run = CommandListCleanupSharedResources\n"
            "endif\n\n"
            "[ResourceTexture0]\nfilename = Textures/Components-0 t=11111111.dds\n"
            "[TextureOverrideTexture0]\nhash = 11111111\nif $object_detected\n    this = ResourceTexture0\nendif\n\n"
            "[ResourceTexture1]\nfilename = Textures/Components-0 t=22222222.dds\n"
            "[TextureOverrideTexture1]\nhash = 22222222\nif $object_detected\n    this = ResourceTexture1\nendif\n\n"
            "[ResourceTexture2]\nfilename = Textures/Components-0 t=33333333.dds\n"
            "[TextureOverrideTexture2]\nhash = 33333333\nif $object_detected\n    this = ResourceTexture2\nendif\n\n"
            "[ResourceTexture3]\nfilename = Textures/Components-1 t=44444444.dds\n"
            "[TextureOverrideTexture3]\nhash = 44444444\nif $object_detected\n    this = ResourceTexture3\nendif\n\n"
            "[ResourceTexture4]\nfilename = Textures/Components-1 t=55555555.dds\n"
            "[TextureOverrideTexture4]\nhash = 55555555\nif $object_detected\n    this = ResourceTexture4\nendif\n"
        )
        ini_path = mod_dir / "mod.ini"
        ini_path.write_text(orig_ini, encoding="utf-8")

        audit_log = root / "audit.log"
        audit_log.write_text(
            "[10:00:00.000] TEX-UHD-AUDIT gen=1 mod=1 guid=54321 comp=0 rtv=7 t0 res=1024x1024 m11 fmt=97 vfmt=98 hash=aaaa0001 rule=00000000 lod=bbbb0001 sub1=00000000 sub2=00000000 via=0 swapped=0 ovr=0\n"
            "[10:00:00.001] TEX-UHD-AUDIT gen=1 mod=1 guid=54321 comp=0 rtv=7 t2 res=512x512 m10 fmt=61 vfmt=61 hash=11111111 rule=11111111 lod=11111111 sub1=00000000 sub2=00000000 via=0 swapped=0 ovr=0\n"
            "[10:00:00.002] TEX-UHD-AUDIT gen=1 mod=1 guid=54321 comp=0 rtv=7 t3 res=1024x1024 m11 fmt=97 vfmt=99 hash=aaaa0003 rule=00000000 lod=bbbb0003 sub1=00000000 sub2=00000000 via=0 swapped=0 ovr=0\n"
            "[10:00:00.003] TEX-UHD-AUDIT gen=1 mod=1 guid=54321 comp=0 rtv=7 t4 res=1024x1024 m11 fmt=97 vfmt=99 hash=aaaa0004 rule=00000000 lod=00000000 sub1=00000000 sub2=00000000 via=0 swapped=0 ovr=1\n"
            "[10:00:00.004] TEX-UHD-AUDIT gen=1 mod=1 guid=54321 comp=1 rtv=7 t3 res=1024x1024 m11 fmt=97 vfmt=99 hash=aaaa0005 rule=00000000 lod=bbbb0005 sub1=00000000 sub2=00000000 via=0 swapped=0 ovr=0\n",
            encoding="utf-8",
        )

        rc = mh.main(
            [
                "--mod-dir",
                str(mod_dir),
                "--audit-log",
                str(audit_log),
                "--out-dir",
                str(out_dir),
                "--no-visual",
            ]
        )
        self.assertEqual(rc, 0)

        # Original mod.ini must remain 100% untouched
        self.assertEqual(ini_path.read_text(encoding="utf-8"), orig_ini)

        mapping = json.loads((out_dir / "slot_mapping.json").read_text(encoding="utf-8"))
        by_key = {(b["comp"], b["logical_key"]): b for b in mapping["bindings"]}

        # Anchor locked ResourceTexture0 (even though its DDS header was BC7_UNORM!)
        self.assertEqual(by_key[(0, "rule:11111111")]["confidence"], "ANCHOR_EXACT")
        self.assertEqual(by_key[(0, "rule:11111111")]["selected_resource"], "ResourceTexture0")

        # Because ResourceTexture0 was locked by anchor, ResourceTexture1 became UNIQUE for comp=0 BC7_UNORM!
        self.assertEqual(by_key[(0, "lod:bbbb0001")]["confidence"], "HIGH_UNIQUE_FORMAT")
        self.assertEqual(by_key[(0, "lod:bbbb0001")]["selected_resource"], "ResourceTexture1")

        # Unique BC7_UNORM_SRGB on comp=0 (t4 was skipped because mod.ini has explicit ps-t4 = null)
        self.assertEqual(by_key[(0, "lod:bbbb0003")]["confidence"], "HIGH_UNIQUE_FORMAT")
        self.assertEqual(by_key[(0, "lod:bbbb0003")]["selected_resource"], "ResourceTexture2")
        self.assertTrue(all(s["slot"] != 4 for b in mapping["bindings"] if b["comp"] == 0 for s in b["slots"]))

        # Component 1 has 2 BC7_UNORM_SRGB files -> MUST stay AMBIGUOUS and disabled
        self.assertEqual(by_key[(1, "lod:bbbb0005")]["confidence"], "AMBIGUOUS")
        self.assertFalse(by_key[(1, "lod:bbbb0005")]["enabled"])
        self.assertIsNone(by_key[(1, "lod:bbbb0005")]["selected_resource"])

        gen_ini = (out_dir / "mod_hash_free.ini").read_text(encoding="utf-8")
        self.assertIn("global $slot_original_format_t0 = 0", gen_ini)
        self.assertIn("run = CommandListBackupPixelShaderResources", gen_ini)
        self.assertIn("run = CommandListSetTexturesComponent0", gen_ini)
        self.assertIn("run = CommandListRestorePixelShaderResources", gen_ini)
        self.assertIn("ps-t4 = null", gen_ini)
        self.assertNotIn("run = CommandListSetTexturesComponent1", gen_ini)

        # Test user --mapping-override roundtrip enabling comp=1 with ResourceTexture4
        by_key[(1, "lod:bbbb0005")]["selected_resource"] = "ResourceTexture4"
        by_key[(1, "lod:bbbb0005")]["enabled"] = True
        ov_path = root / "override.json"
        ov_path.write_text(json.dumps(mapping, indent=2), encoding="utf-8")
        out_dir2 = root / "out2"
        rc2 = mh.main(
            [
                "--mod-dir",
                str(mod_dir),
                "--audit-log",
                str(audit_log),
                "--out-dir",
                str(out_dir2),
                "--no-visual",
                "--mapping-override",
                str(ov_path),
                "--comment-legacy-tex-overrides",
            ]
        )
        self.assertEqual(rc2, 0)
        gen_ini2 = (out_dir2 / "mod_hash_free.ini").read_text(encoding="utf-8")
        self.assertIn("run = CommandListSetTexturesComponent1", gen_ini2)
        self.assertIn("ps-t3 = ref ResourceTexture4", gen_ini2)
        self.assertIn("; [legacy-hash-disabled] [TextureOverrideTexture0]", gen_ini2)

    def test_max_weight_assignment_and_sig_comparison(self) -> None:
        # Verify exact 1-to-1 max-weight assignment prefers global optimum over greedy local peak
        edges = {
            (0, 0): 0.90,
            (0, 1): 0.85,
            (1, 0): 0.88,
            (1, 1): 0.10,
        }
        assign = mh.solve_max_weight_assignment(2, 2, edges)
        self.assertEqual(assign, {0: 1, 1: 0})

        # Verify identical 5-patch signatures score SSIM ~ 1.0, hist = 1.0
        px = bytes((i * 17) & 0xFF for i in range(tl.K_PATCHES * 16 * 16 * 4))
        sig_a = mh.DdsVisualSig(codec=7, nch=4, pw=16, ph=16, px=px, rgb_mean=(120, 120, 120), rgb_std=(60, 60, 60))
        sig_b = mh.DdsVisualSig(codec=7, nch=4, pw=16, ph=16, px=px, rgb_mean=(120, 120, 120), rgb_std=(60, 60, 60))
        ssim, hist = mh.compare_two_sigs(sig_a, sig_b)
        self.assertGreater(ssim, 0.999)
        self.assertAlmostEqual(hist, 1.0, places=5)


if __name__ == "__main__":
    unittest.main()
