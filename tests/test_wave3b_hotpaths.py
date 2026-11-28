from __future__ import annotations

import pathlib
import subprocess
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
CPP_SRC = REPO_ROOT / "tests" / "test_wave3b_hotpaths.cpp"


class Wave3BHotpathTests(unittest.TestCase):
    def test_wave3b_cpp_concurrency_and_models(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            exe = pathlib.Path(td) / "test_wave3b_hotpaths"
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
            self.assertIn("OK: test_wave3b_hotpaths", run_res.stdout)

    def test_wave3b_source_contracts(self) -> None:
        # 1. Host header copies stay identical and use flat atomic heap array
        host_hdr = (REPO_ROOT / "source/host/source/d3d12/d3d12_impl_device.hpp").read_text()
        inc_hdr = (REPO_ROOT / "source/include/d3d12_impl_device.hpp").read_text()
        self.assertEqual(host_hdr, inc_hdr)
        self.assertIn("kFastDescriptorHeapCount = 16384", host_hdr)
        self.assertNotIn("concurrent_vector", host_hdr)

        # 2. Main addon caches immutable is_vis and guards all 3 TryFreeze* readers
        core_inc = (REPO_ROOT / "source/main/reshade_addon/hooks/descriptor_publish_core.inc").read_text()
        copy_inc = (REPO_ROOT / "source/main/reshade_addon/hooks/descriptor_publish_copy.inc").read_text()
        self.assertIn("const bool is_vis;", core_inc)
        self.assertIn("if(s.is_vis||!PreviousCurrent(pair,s))return false;", copy_inc)
        self.assertIn("if(!h||h->is_vis||!PreviousCurrent(pair,*h))return false;", copy_inc)
        self.assertIn("if(!h||h->is_vis||!count||first>=h->flat.size||count>h->flat.size-first)return false;", copy_inc)
        self.assertIn("if(!dh->is_vis){", copy_inc)
        self.assertIn("pg.seq.store(sq+1u,std::memory_order_release);", copy_inc)

        # 3. Client uses PrimaryRuntimeTable + off-lock DXR build + deferred mod_stamp
        runtime_cpp = (REPO_ROOT / "source/client/hsin_runtime.cpp").read_text()
        self.assertIn("PrimaryRuntimeTable<Runtime,api::device *,std::uint64_t> primary_runtime;", runtime_cpp)
        self.assertIn("RaygenIdentity::build_native_unlocked", runtime_cpp)
        self.assertIn("r.reset_recording(c)", runtime_cpp)
        self.assertIn("if(mod_stamp&&stamp.list)mod_stamp(stamp.list,stamp.creation,stamp.generation);", runtime_cpp)

        # 4. Wave 3C contracts: per-component settled recipe caching, private PS-slot fast path,
        # live CustomResource handle reads, and confirmed-no-PS-rules TLS memo.
        fb_hdr = (REPO_ROOT / "source/main/reshade_addon/textures/tex_struct_fallback.hpp").read_text()
        self.assertIn("IsCompEvidenceSettled(load, dm, comp)", fb_hdr)
        self.assertIn("comp_evaluated_seq_", fb_hdr)
        ps_stamp = (REPO_ROOT / "source/main/reshade_addon/draw/section_ps_stamp.cpp").read_text()
        self.assertIn("PlanPrivateSectionPsSlots(", ps_stamp)
        self.assertIn("RecordNativeReadAsBound(", ps_stamp)
        gate_cpp = (REPO_ROOT / "source/main/reshade_addon/rt/rt_raster_gate.cpp").read_text()
        self.assertIn("RecordNativeReadAsBound(list);", gate_cpp)
        hyb_inc = (REPO_ROOT / "source/main/reshade_addon/rt/rt_hybrid_main.inc").read_text()
        self.assertIn("PortApi::FindCustomResourceHandle(", hyb_inc)
        self.assertIn("PortApi::ReadBufferResourceHandle(", hyb_inc)


if __name__ == "__main__":
    unittest.main()
