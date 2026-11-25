# Third-Party Notices & Provenance

**WWMI-DX12-RT** is an open-source project distributed as a whole under the **GNU General Public License v3.0 (`GPL-3.0`)** (see [`LICENSE`](./LICENSE)).

All code authored specifically for this project (`source/client/`, `source/include/`, `source/shaders/`, `source/linux/`, `tools/`, `tests/`, and modifications in `source/host/` & `source/main/`) is licensed under **`GPL-3.0`**; bundled upstream and third-party components retain their original compatible licenses listed below.

---

## 1. Upstream Base Projects & Major Components

| Component | Upstream Repository / Baseline | License | License Text in Repository / Release Package | Role in WWMI-DX12-RT |
|---|---|---|---|---|
| **wwmi-dx12** | [`github.com/lekarsenten/wwmi-dx12`](https://github.com/lekarsenten/wwmi-dx12) (`2026.10.4`, commit `ec837ef8ab798b7c251105a7448efeceb526a2c9`) | `GPL-3.0` | `LICENSE` | Base DirectX 12 WWMI add-on framework, mod loader, DX12 command/resource wrappers, base `TexLook`/`TexLod` caches, and 3 base compute shaders (`Bc7Half.cso`, `BufCopy.cso`, `IbVbHash.cso`). Extended and modified in `source/main/`. |
| **3Dmigoto (XXMI Fork)** | [`github.com/SpectrumQT/3Dmigoto`](https://github.com/SpectrumQT/3Dmigoto) (commit `a7847eae`, via `wwmi-dx12`) | `GPL-3.0` | `payload/licenses/3Dmigoto-LICENSE-GPL3.txt`, `3Dmigoto-COPYING.txt`, `3Dmigoto-AUTHORS.txt` | INI parser, keybinding state machine, command-list execution engine (`migoto_engine`), and modified D3D12 bridge shims compiled into `wwmi_dx12.addon64`. |
| **ReShade & ReShade Add-on SDK** | [`github.com/crosire/reshade`](https://github.com/crosire/reshade) — (c) Patrick Mours | `BSD-3-Clause` | `payload/licenses/ReShade-LICENSE-BSD3.txt` | Base D3D12/DXGI host (`ReShade64-game-forward.dll` / `dxgi.dll`, extended in `source/host/` with acceleration-structure slot tracking, copy-merge, and compute state forwarding) and add-on SDK headers. |
| **DirectX Shader Compiler (DXC)** | [`github.com/microsoft/DirectXShaderCompiler`](https://github.com/microsoft/DirectXShaderCompiler) (`1.8.2502`, unmodified Microsoft-signed runtime `dxcompiler.dll` & `dxil.dll`) | LLVM/NCSA, MIT & Microsoft Distributable Code License | `payload/licenses/dxc/LICENSE-LLVM.txt`, `LICENSE-MIT.txt`, `LICENSE-MS.txt`, `ThirdPartyNotices.txt` | Unmodified Microsoft-signed runtime shader compiler/validator shipped in `reshade-addons/wwmi_dx12/dxc/` for RabbitFX and DXIL contract inspection. |
| **Dear ImGui & ProggyClean Font** | [`github.com/ocornut/imgui`](https://github.com/ocornut/imgui) — (c) Omar Cornut; embedded `ProggyClean.ttf` — (c) Tristan Grimmer | `MIT` | `payload/licenses/ImGui-LICENSE-MIT.txt`, `payload/licenses/ProggyClean-LICENSE-MIT.txt` | Overlay UI and default embedded bitmap font compiled into the host / add-on SDK. |
| **Fork Awesome** | [`forkawesome.github.io`](https://forkawesome.github.io) (embedded via ReShade `source/fonts/forkawesome.inl`) | `SIL OFL-1.1` (font) / `MIT` (code) | `payload/licenses/ForkAwesome-LICENSE-OFL.txt` | Embedded icon font used by the ReShade overlay UI. |
| **MinHook** | ReShade `deps/minhook` at `8fda4f5481fed5797dc2651cd91e238e9b3928c6` | BSD-2-Clause (including HDE notices) | `payload/licenses/MinHook-LICENSE.txt` | Static Windows hooking library. |
| **GLAD / Khronos specifications** | ReShade `deps/glad` at `27bed1181560211b55e39a9b132fef8c5846aae5`, `deps/khronos` and `source/host/deps/glad/target` | MIT, Apache-2.0 and Khronos terms as marked | `payload/licenses/GLAD-LICENSE.txt`, `Apache-2.0.txt` | Generated static OpenGL, Vulkan and WGL loaders. |
| **fpng** | ReShade `deps/fpng` | Unlicense | `payload/licenses/fpng-UNLICENSE.txt` | Static PNG codec. |
| **Simple Lossless JPEG XL** | ReShade `deps/jxl_simple_lossless` | BSD-3-Clause and patent grant | `payload/licenses/JXL-LICENSE.txt`, `JXL-PATENTS.txt` | Static JPEG XL codec. |
| **stb** | ReShade `deps/stb` | MIT or public-domain dedication | `payload/licenses/stb-LICENSE.txt` | Static image read/write and resizing helpers. |
| **DirectX-Headers, OpenVR, OpenXR, SPIRV-Headers, utfcpp and VMA** | Pinned ReShade `deps/` | See individual licence text | `payload/licenses/ReShade-deps/` | Header dependencies included in the ReShade build. |
| **PCRE2 10.30** | Pinned [3Dmigoto source and licence](https://github.com/SpectrumQT/3Dmigoto/tree/a7847eae82aaac32175c9a7d199673d5c9323bcd/pcre2-10.30), with the x64 library in `pcre2/pcre2-8-64.lib` | `BSD (PCRE2)` | `payload/licenses/PCRE2-LICENCE-BSD.txt` | Regular expression matching compiled into `wwmi_dx12.addon64` via `3Dmigoto`. |
| **DirectXTK (subset)** | [`github.com/microsoft/DirectXTK`](https://github.com/microsoft/DirectXTK) — (c) Microsoft Corporation (via `3Dmigoto`) | `MIT` | `payload/licenses/DirectXTK-LICENSE-MIT.txt` | DDS texture loader helpers used via `3Dmigoto`. |
| **crc32c-hw 1.0.5** | Via `3Dmigoto` (`source/client/crc32c.cpp`) | `zlib` | `payload/licenses/crc32c-LICENSE.txt` | Hardware-accelerated CRC32C calculation. |
| **bcdec** | [`github.com/iOrange/bcdec`](https://github.com/iOrange/bcdec) | `MIT` or `Unlicense` | `payload/licenses/bcdec-LICENSE.txt` | Single-header BCn block decompression used in `tex_look.cpp` and `tools/third_party/bcdec.h`. |
| **NVIDIA Streamline SDK (headers)** | [`github.com/NVIDIAGameWorks/Streamline`](https://github.com/NVIDIAGameWorks/Streamline) (`v2.10.0` C API headers) | `MIT` | `payload/licenses/Streamline-LICENSE-MIT.txt` | Passive DLSS-G frame-generation query struct definitions included in `source/host/source/upstream-v2.10.0/`. |

The release's `BUILD-PROVENANCE.json` identifies the source checkout, upstream
pins and build entry points. PCRE2's complete source licence is
`pcre2-10.30/LICENCE` (and complete PCRE2 10.30 C source is under `pcre2-10.30/`)
in the pinned 3Dmigoto tree. `wwmi_dx12.addon64` compiles against the minimal
GPL-3.0 header stub `reshade_addon/migoto/NktHookLib.h` (via `source/main/3dm_override/DLLMainHook.h`)
and does **not** link `Nektra/NktHookLib64.lib`.

---

## 2. Summary of Modifications & Original Additions Over Upstream

### 2.1 `source/host/` — Modified `ReShade` Host (`BSD-3-Clause`)
Derived from `crosire/reshade` and modified (2026-11) to add:
- D3D12 BLAS/TLAS acceleration structure slot observation & asynchronous copy-merge (`wwmi_as_slots.hpp`, `wwmi_as_copy_merge.hpp`);
- D3D12 compute root-argument and raytracing state-object forwarding exports (`wwmi_rt_forward.cpp`, `wwmi_compute_state.cpp`, `d3d12_device.cpp`, `d3d12_impl_device.cpp`, `d3d12_command_list.cpp`, `d3d12_impl_command_list.cpp`, `dxgi_swapchain.cpp`);
- Passive non-intrusive host startup, deterministic sorted add-on loading, and frame-generation swapchain compatibility (`dll_main.cpp`, `addon_manager.cpp`).

### 2.2 `source/main/` — Modified & Extended `wwmi-dx12` Provider (`GPL-3.0`)
Derived from `lekarsenten/wwmi-dx12` (`2026.10.4`) and `SpectrumQT/3Dmigoto` (`a7847eae`), modified and extended (2026-11) with:
- External `<BasePath>/wwmi_dx12.ini` zero-env bootstrap, process whitelist guard, log rotation, and zero-write `Win64` isolation (`wwmi_addon.cpp`, `pack-frame.inc`, `present_hook.cpp`);
- Hardware Ray Tracing + Hybrid Raster gate, DXIL program contracts, descriptor copy-publication, and deformed/base-skin vertex/16-bit IB routing (`rt_raster_gate.cpp`, `rt_program_contracts.cpp`, `rt_program_runtime.cpp`, `rt_localvf_inputs.cpp`, `rt_hybrid_main.inc`, `descriptor_publish_*.inc`, `rabbitfx_bind.cpp`, `CommandList.cpp`, `dx12_context_impl.cpp`, `dx12_device_impl.cpp`, `dx12_pipeline.cpp`, `dx12_wrap.cpp`);
- Route 3 modular `TexLook`/`TexLod` seed overlays and zero-touch structural + 5-patch visual texture auto-adaptation for 4K UHD (`-krqlv=uhd`) (`tex_struct_fallback.hpp`, `texture_module.cpp`, `tex_look.cpp`, `tex_lod.cpp`).

### 2.3 `source/client/`, `source/include/`, `source/shaders/`, `source/linux/`, `tools/` — Project Modules (`GPL-3.0`)
The following project-specific components are licensed under `GPL-3.0` (excluding the third-party `crc32c.cpp`, `bcdec.h`, and ReShade-derived headers listed in Section 1):
- **`zz_wwmi_rt_pack.addon64` (`source/client/`, `source/include/`, `source/shaders/rt_pack/`)**: Data-driven DirectX Raytracing (DXR) + Hybrid Raster consumer, GPU skinning/shapekey compute pipelines, BLAS/TLAS/SBT patching, and automatic `NativeDiscovery` profile learning;
- **Cross-Platform Installer & Tooling (`tools/`, `scripts/wwmi-dx12-setup.py`, `source/linux/wine-filename-utf8.c`)**: 2-file `BasePath` ledger-backed installer/uninstaller, zero-dependency Win32 High-DPI GUI/CLI configurator (`WWMI-DX12-RT-Setup.exe`), offline Hash-Free mod migrator, modular seed manager, and Linux Wine UTF-8 filename shim.

---

## 3. Upstream Credits & Acknowledgements

WWMI-DX12-RT stands on the shoulders of the open-source modding and graphics community:
- **lekarsenten** — Author of upstream [`wwmi-dx12`](https://github.com/lekarsenten/wwmi-dx12)
- **Patrick Mours ([crosire](https://github.com/crosire))** — Author of [ReShade](https://reshade.me) and the ReShade Add-on API
- **[SpectrumQT](https://github.com/SpectrumQT)** & **SinsOfSeven** — Authors of [WWMI](https://github.com/SpectrumQT/WWMI-Package), [XXMI Launcher](https://github.com/SpectrumQT/XXMI-Launcher), and the XXMI 3Dmigoto fork
- **Chiri**, **[Bo3b](https://github.com/bo3b)**, **[DarkStarSword](https://github.com/DarkStarSword)** — Original authors of 3Dmigoto
- **SilentNightSound** & **Nurarihyon** — Early Wuthering Waves 3Dmigoto fork & XXMI DLL engineering
- **caverabbit** — Author of RabbitFX
