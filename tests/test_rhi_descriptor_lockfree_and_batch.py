from __future__ import annotations

import pathlib
import subprocess
import tempfile
import unittest

REPO_ROOT = pathlib.Path(__file__).resolve().parents[1]
CPP_SRC = REPO_ROOT / "tests" / "test_rhi_descriptor_lockfree_and_batch.cpp"


class RhiDescriptorLockFreeAndBatchTests(unittest.TestCase):
    def test_cpp_concurrency_refcache_and_gpu_addr_cache(self) -> None:
        with tempfile.TemporaryDirectory(prefix="wwmi_rhi_opt_test_") as td:
            exe = pathlib.Path(td) / "test_rhi_descriptor_lockfree_and_batch"
            comp = subprocess.run(
                [
                    "g++",
                    "-std=c++20",
                    "-O2",
                    "-Wall",
                    "-Wextra",
                    "-pthread",
                    f"-I{REPO_ROOT / 'source' / 'include'}",
                    str(CPP_SRC),
                    "-o",
                    str(exe),
                ],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(
                comp.returncode,
                0,
                msg=f"g++ failed:\nstdout:\n{comp.stdout}\nstderr:\n{comp.stderr}",
            )
            run = subprocess.run([str(exe)], capture_output=True, text=True, check=False, timeout=60)
            self.assertEqual(
                run.returncode,
                0,
                msg=f"test failed:\nstdout:\n{run.stdout}\nstderr:\n{run.stderr}",
            )
            self.assertIn("OK: test_rhi_descriptor_lockfree_and_batch", run.stdout)

    def test_rhi_reduction_abi_and_source_contracts(self) -> None:
        # 1. Host & include d3d12_impl_device.hpp remain byte-for-byte identical
        host_hdr = (REPO_ROOT / "source/host/source/d3d12/d3d12_impl_device.hpp").read_text()
        inc_hdr = (REPO_ROOT / "source/include/d3d12_impl_device.hpp").read_text()
        self.assertEqual(host_hdr, inc_hdr)
        self.assertIn("fast_descriptor_heap_at", host_hdr)

        # 2. v3 batch descriptor publisher ABI keeps v1/v2 compatibility and wires copy_batch
        abi_hdr = (REPO_ROOT / "source/include/wwmi_descriptor_publish.h").read_text()
        self.assertIn("WWMI_DESCRIPTOR_BATCH_VERSION = 3", abi_hdr)
        self.assertIn("WWMI_DescriptorPublisher_v3", abi_hdr)
        pub_inc = (REPO_ROOT / "source/host/source/d3d12/descriptor_publish.inc").read_text()
        self.assertIn("observe_copy_batch", pub_inc)
        self.assertIn("WWMI_DESCRIPTOR_BATCH_VERSION", pub_inc)

        # 3. Main addon implements per-page writer-only vis_write_lock + zero-LOCK
        #    WriteSlotUnderPageLock, no-write-allocate LaneRefCache (512 sets, noinline),
        #    wait-free TLS abandonment, callback_depth reclamation under lane exhaustion,
        #    WithPublishedSlot reset barrier, LaneRefCache flush on reset/stop, and
        #    OnNativeDescriptorCopyBatch
        native_inc = (REPO_ROOT / "source/main/reshade_addon/hooks/descriptor_native_access.inc").read_text()
        self.assertIn("kCacheDrainBit", native_inc)
        self.assertIn("kLaneAbandoned", native_inc)
        self.assertIn("FlushAbandonedLaneRefCaches", native_inc)
        self.assertIn("FlushAllLaneRefCaches", native_inc)
        content_inc = (REPO_ROOT / "source/main/reshade_addon/hooks/descriptor_content.inc").read_text()
        self.assertIn("struct LaneRefCache", content_inc)
        self.assertIn("kSets=512", content_inc)
        self.assertIn("try_retain_miss", content_inc)
        self.assertIn("release_miss", content_inc)
        self.assertIn("kSlotUpdatingBit", content_inc)
        self.assertIn("kMaxBorrowRefCount", content_inc)
        core_inc = (REPO_ROOT / "source/main/reshade_addon/hooks/descriptor_publish_core.inc").read_text()
        self.assertIn("vis_write_lock", core_inc)
        self.assertIn("struct VisWriteGuard", core_inc)
        self.assertIn("WriteSlotUnderPageLock", core_inc)
        self.assertIn("WithPublishedSlot", core_inc)
        self.assertIn("ReadPublishedSlot", core_inc)
        self.assertIn("FlushAllLaneRefCaches();", core_inc)
        copy_inc = (REPO_ROOT / "source/main/reshade_addon/hooks/descriptor_publish_copy.inc").read_text()
        self.assertIn("OnNativeDescriptorCopyBatch", copy_inc)
        self.assertIn("FlushAbandonedLaneRefCaches();", copy_inc)
        self.assertIn("all_dst_vis", copy_inc)
        self.assertIn("lock_vis_write", copy_inc)
        views_inc = (REPO_ROOT / "source/main/reshade_addon/hooks/descriptor_publish_views.inc").read_text()
        self.assertIn("WWMI_DESCRIPTOR_BATCH_VERSION", views_inc)
        self.assertIn("FlushAllLaneRefCaches();", views_inc)
        stamp_inc = (REPO_ROOT / "source/main/reshade_addon/hooks/descriptor_publish_stamp.inc").read_text()
        self.assertIn("if(!ok){heap_stale=true;break;}", stamp_inc)
        self.assertIn("pair.epoch", stamp_inc)
        rt_cpp = (REPO_ROOT / "source/main/reshade_addon/hooks/resource_tracker.cpp").read_text()
        self.assertIn("pair.epoch!=expected_epoch", rt_cpp)

        # 4. Host CopyDescriptors short-circuits !touches_as and uses copy_batch
        dev_cpp = (REPO_ROOT / "source/host/source/d3d12/d3d12_device.cpp").read_text()
        self.assertIn("wwmi::descriptor_publish::copy_batch", dev_cpp)
        self.assertIn("if (!touches_as)", dev_cpp)

        # 5. Client InputCollector & NativeDiscovery & Hybrid lock batching
        inputs_hpp = (REPO_ROOT / "source/client/hsin_inputs.hpp").read_text()
        inputs_cpp = (REPO_ROOT / "source/client/hsin_inputs.cpp").read_text()
        disc_hpp = (REPO_ROOT / "source/client/native_discovery.hpp").read_text()
        hyb_inc = (REPO_ROOT / "source/client/rt_hybrid_client.inc").read_text()
        hyb_assets = (REPO_ROOT / "source/client/rt_hybrid_assets.hpp").read_text()
        self.assertIn("active_binding_lists_", inputs_hpp)
        self.assertIn("shaders_seq_", inputs_hpp)
        self.assertIn("DiscoveryInterestsSnapshot", inputs_hpp)
        self.assertIn("query_hybrid_preflight", inputs_hpp)
        self.assertIn("query_hybrid_liveness", inputs_hpp)
        self.assertIn("query_hybrid_previous", inputs_hpp)
        self.assertIn("if(active_binding_lists_.load(std::memory_order_acquire)==0)return false;", inputs_cpp)
        self.assertIn("cached_interest_snap_", disc_hpp)
        self.assertIn("inputs.query_hybrid_preflight(", hyb_inc)
        self.assertIn("inputs.query_hybrid_liveness(", hyb_inc)
        self.assertIn("inputs.query_hybrid_previous(", hyb_inc)
        self.assertIn("std::unordered_map<Key,Window,KeyHash>", hyb_assets)

        # 6. Generic F6=ON engine hot paths: wrap TLS cache, OwnedBuffer TLS cache,
        #    batched BindDescriptors CopyDescriptors, TypedTrampoline name cache,
        #    layout table/b2 epochs, and 512-entry material note dedup cache
        wrap_cpp = (REPO_ROOT / "source/main/reshade_addon/migoto/dx12_wrap.cpp").read_text()
        pipe_cpp = (REPO_ROOT / "source/main/reshade_addon/migoto/dx12_pipeline.cpp").read_text()
        disp_cpp = (REPO_ROOT / "source/main/reshade_addon/migoto/port_dispatch.cpp").read_text()
        lay_cpp = (REPO_ROOT / "source/main/reshade_addon/mods/layout_registry.cpp").read_text()
        comp_cpp = (REPO_ROOT / "source/main/reshade_addon/draw/draw_comp.cpp").read_text()
        note_inc = (REPO_ROOT / "source/include/rt_material_draw_note.inc").read_text()
        self.assertIn("g_wrap_gen", wrap_cpp)
        self.assertIn("g_owned_bufs_gen", pipe_cpp)
        self.assertIn("CopyDescriptors(copy_n, copy_dsts, kOnes", pipe_cpp)
        self.assertIn("struct NameCache", disp_cpp)
        self.assertIn("g_layout_tables_epoch", lay_cpp)
        self.assertIn("g_layout_b2_epoch", lay_cpp)
        self.assertIn("std::pair<UINT, uint64_t>        tables[64]", comp_cpp)
        self.assertIn("std::array<RecentSet,256>", note_inc)


if __name__ == "__main__":
    unittest.main()
