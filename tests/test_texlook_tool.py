#!/usr/bin/env python3
"""Automated unit tests for tools/texlook_tool.py."""
from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))

import texlook_tool as tl  # noqa: E402


def _make_ref(target: int, pw: int, ph: int, codec: int = 7, nch: int = 4, fill: int = 0x55) -> tl.LookRef:
    w = pw * 16
    h = ph * 16
    px = bytes([fill]) * (tl.K_PATCHES * pw * ph * nch)
    return tl.LookRef(target=target, codec=codec, nch=nch, w=w, h=h, pw=pw, ph=ph, px=px)


class TestTexLookTool(unittest.TestCase):
    def test_fnv_helpers_match_cpp(self) -> None:
        # Known golden checks for FNV-1a 64-bit arithmetic wrapping
        p1 = tl.fnv_mesh_pass_of(0x91CFF509, 75486, 115707, 0x123456789ABCDEF0)
        p2 = tl.fnv_slot_of(p1, 3)
        self.assertNotEqual(p1, 0)
        self.assertNotEqual(p2, p1)
        self.assertEqual(tl.fnv_place_of("Mods\\TestMod", 2, 0xABCD), tl.fnv_place_of("mods\\testmod", 2, 0xABCD))

    def test_texlook_roundtrip_and_merge_semantics(self) -> None:
        d = Path(tempfile.mkdtemp(prefix="wwmi-test-"))
        a = tl.TexLookData(
            gen=10,
            refs={
                0x11111111: _make_ref(0x11111111, 16, 16, fill=1),
                0x22222222: _make_ref(0x22222222, 32, 32, fill=2),
            },
            verdicts={0xAAAAAAAA: tl.Verdict(0xAAAAAAAA, 10, 1)},
            places={0x100: 0x11111111, 0x101: 0x22222222},
            mesh_places={0x200: 0x11111111, 0x201: 0x22222222},
        )
        b = tl.TexLookData(
            gen=12,
            refs={
                0x11111111: _make_ref(0x11111111, 32, 32, fill=9),  # denser -> should upgrade
                0x22222222: _make_ref(0x22222222, 16, 16, fill=8),  # weaker -> should keep 32x32
                0x33333333: _make_ref(0x33333333, 32, 32, fill=3),  # new
            },
            verdicts={0xBBBBBBBB: tl.Verdict(0xBBBBBBBB, 12, 0)},
            places={0x100: 0x11111111, 0x101: 0x33333333},  # 0x101 conflict -> kMixed
            mesh_places={0x200: 0x11111111, 0x202: 0x33333333},
        )
        pa, pb, pm = d / "a.texlook", d / "b.texlook", d / "merged.texlook"
        tl.write_texlook(pa, a)
        tl.write_texlook(pb, b)

        ra = tl.read_texlook(pa)
        self.assertEqual(len(ra.refs), 2)
        self.assertEqual(ra.refs[0x11111111].pw, 16)
        self.assertEqual(ra.mesh_places[0x200], 0x11111111)

        rc = tl.main(["merge", "--out", str(pm), "--drop-verdicts", str(pa), str(pb)])
        self.assertEqual(rc, 0)
        rm = tl.read_texlook(pm)
        self.assertEqual(len(rm.refs), 3)
        self.assertEqual(rm.refs[0x11111111].pw, 32)
        self.assertEqual(rm.refs[0x11111111].px[0], 9)
        self.assertEqual(rm.refs[0x22222222].pw, 32)
        self.assertEqual(rm.refs[0x22222222].px[0], 2)
        self.assertEqual(len(rm.verdicts), 0)
        self.assertEqual(rm.places[0x100], 0x11111111)
        self.assertEqual(rm.places[0x101], tl.K_MIXED)
        self.assertEqual(rm.mesh_places[0x202], 0x33333333)

    def test_lodcache_roundtrip_and_merge_semantics(self) -> None:
        d = Path(tempfile.mkdtemp(prefix="wwmi-test-"))
        p1 = d / "s1.lodcache"
        p2 = d / "s2.lodcache"
        pm = d / "merged.lodcache"
        p1.write_text(
            "11111111 = aaaaaaaa mip=1 exact=1 chain=0 | bbbbbbbb mip=0 exact=0 chain=0 ssim=0.950\n"
            "22222222 = cccccccc mip=1 exact=1 chain=0\n",
            encoding="utf-8",
        )
        p2.write_text(
            "11111111 = bbbbbbbb mip=2 exact=1 chain=0\n"  # promotes look-alike to exact
            "33333333 = cccccccc mip=1 exact=1 chain=0\n"  # conflicts with 22222222 -> degenerate
            "degenerate = dddddddd\n",
            encoding="utf-8",
        )
        rc = tl.main(["merge", "--out", str(pm), str(p1), str(p2)])
        self.assertEqual(rc, 0)
        merged = tl.read_lodcache(pm)
        self.assertIn(0xAAAAAAAA, merged.records)
        self.assertTrue(merged.records[0xBBBBBBBB][0].exact)
        self.assertEqual(merged.records[0xBBBBBBBB][0].mip, 2)
        self.assertNotIn(0xCCCCCCCC, merged.records)
        self.assertIn(0xCCCCCCCC, merged.degenerate)
        self.assertIn(0xDDDDDDDD, merged.degenerate)

    def test_export_role_and_audit_report(self) -> None:
        d = Path(tempfile.mkdtemp(prefix="wwmi-test-"))
        mod_ini = d / "mod.ini"
        mod_ini.write_text(
            "[Constants]\n"
            "global $object_guid = 99999\n"
            "[TextureOverrideComponent0]\n"
            "hash = 12345678\n"
            "match_first_index = 0\n"
            "match_index_count = 300\n"
            "[TextureOverrideTexture0]\n"
            "hash = 00001001\n",
            encoding="utf-8",
        )
        audit_log = d / "test.log"
        audit_log.write_text(
            "[10:00:00.000] TEX-UHD-AUDIT gen=1 mod=1 guid=99999 comp=0 rtv=7 t0 res=1024x1024 m11 fmt=97 vfmt=99 hash=aaaa0001 rule=00000000 lod=00002002 sub1=00000000 sub2=00000000 via=0 swapped=0 ovr=0\n"
            "[10:00:00.001] TEX-UHD-AUDIT gen=1 mod=1 guid=99999 comp=0 rtv=7 t2 res=512x512 m10 fmt=61 vfmt=61 hash=00001001 rule=00001001 lod=00001001 sub1=00000000 sub2=00000000 via=0 swapped=0 ovr=0\n",
            encoding="utf-8",
        )
        seed_path = d / "seed.texlook"
        tl.write_texlook(
            seed_path,
            tl.TexLookData(
                gen=5,
                refs={
                    0x00002002: _make_ref(0x00002002, 32, 32, fill=7),
                    0x00009999: _make_ref(0x00009999, 32, 32, fill=8),  # unrelated role
                },
                mesh_places={0x5555: 0x00002002, 0x6666: 0x00009999},
            ),
        )
        out_role = d / "role.texlook"
        rc = tl.main(
            [
                "export-role",
                "--texlook",
                str(seed_path),
                "--mod-ini",
                str(mod_ini),
                "--audit-log",
                str(audit_log),
                "--out",
                str(out_role),
            ]
        )
        self.assertEqual(rc, 0)
        role_tl = tl.read_texlook(out_role)
        self.assertEqual(set(role_tl.refs.keys()), {0x00002002})
        self.assertEqual(role_tl.mesh_places, {0x5555: 0x00002002})

        rep_json = d / "report.json"
        rc = tl.main(
            [
                "report",
                "--audit-log",
                str(audit_log),
                "--texlook",
                str(seed_path),
                "--mod-ini",
                str(mod_ini),
                "--json",
                str(rep_json),
            ]
        )
        self.assertEqual(rc, 0)
        rep = json.loads(rep_json.read_text(encoding="utf-8"))
        self.assertEqual(rep["status_summary"]["DRIFTED_SEED_RECOVERED"], 1)
        self.assertEqual(rep["status_summary"]["MATCHED_EXACT"], 1)


if __name__ == "__main__":
    unittest.main()
