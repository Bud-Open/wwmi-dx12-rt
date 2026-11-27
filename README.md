# WWMI-DX12-RT

**English** | [简体中文](./README.zh-CN.md)

[![License: GPL-3.0](https://img.shields.io/badge/License-GPL--3.0-blue.svg)](./LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux%20%2F%20SteamOS-informational)](#quick-start--uninstall)
[![Graphics API](https://img.shields.io/badge/API-DirectX%2012%20%2B%20DXR-success)](#texture-pack--graphics-settings-compatibility)

An open-source WWMI runtime and cross-platform external mounting toolchain for *Wuthering Waves* on **native DirectX 12 + hardware Ray Tracing (DXR) + DLSS Frame Generation + 4K UHD (`-krqlv=uhd`)**.

Built on [`lekarsenten/wwmi-dx12`](https://github.com/lekarsenten/wwmi-dx12) (`GPL-3.0`) and [`crosire/reshade`](https://github.com/crosire/reshade) (`BSD-3-Clause`), this project adds a **hardware Ray Tracing consumer backend (`zz_wwmi_rt_pack`), a Route 3 structural + visual UHD texture matcher, deterministic sorted add-on loading, and a 2-file external mount architecture**.

> **Note**: This repository provides the runtime, ray-tracing consumer, and installers only. It does **not** include game files or character skin mods.

---

## Key Features

- **Native DX12 + Hardware Ray Tracing + Frame Generation**: Character mods render cleanly across hybrid raster passes and DXR ray-traced reflections/GI while keeping DLSS Frame Generation and 4K UHD intact—no downgrade to HD and no fallback to DX11.
- **Compatible with Existing WWMI Mods**: Directly loads standard `WWMI` trees (`d3dx.ini`, `Core/WWMI/`, `Mods/RabbitFX/`, `Mods/*/`, and nested mod collections managed by `XXMI Launcher` or `IMM`) without converting mod files.
- **Live Controls That Actually Work in RT Mode**:
  - **`F10`**: Hot-reload all mods and `.ini` files in-game without restarting.
  - **`F6`**: Live toggle between modded and vanilla rendering across both hybrid raster and DXR reflections.
  - **`Home`**: ReShade overlay with live `wwmi-dx12` RT status (`DX12 Ray Tracing + Hybrid Raster active`) and active mod count.
- **Install Once, Launch Normally**:
  - **Windows (Official Kuro Launcher / Steam)**: Mount once with the standalone Win32 GUI setup tool, then launch *Wuthering Waves* directly from the official launcher or Steam.
  - **Linux / SteamOS / Steam Deck (Proton)**: Mount once via GUI or CLI, set Steam Launch Options once (`WINEDLLOVERRIDES="dxgi=n,b" %command%`), and click **Play** in Steam.
- **2-File Zero-Pollution External Mount (`BasePath`)**:
  - Places only **2 bootstrap files** (`dxgi.dll` + 2-line redirect `ReShade.ini`) inside `Client/Binaries/Win64/`, guarded by an executable allowlist (`Client-Win64-Shipping.exe` only—never hooks launcher executables).
  - Add-ons (`.addon64`), RT shaders, `wwmi_dx12.ini`, caches, persistent toggles (`runtime/d3dx_user.ini`), and rotated logs stay **outside the game directory** (zero runtime writes to `Win64`).
  - **Non-destructive uninstall**: Ledger-verified rollback stages only the 2 owned files and refuses to overwrite unrelated third-party `dxgi.dll` / `ReShade.ini` files.
- **Self-Contained Open-Source Build Chain**: `CMake + Ninja` builds on Windows and Linux cross-compile with **zero proprietary Nektra linkage** (`NktHookLib64.lib` removed) and full reproducible `BUILD-PROVENANCE.json` + third-party license bundles.

---

## Prerequisites

Before installing, make sure you have:

1. **DirectX 12 mode enabled** in *Wuthering Waves* (do **not** enable DX11 mode).
2. A **WWMI mod directory**:
   - If you already use `XXMI Launcher`, the installer detects your existing `WWMI` directory automatically.
   - If you are starting fresh, use **Init Skeleton** in the installer, download the official [`WWMI-Package`](https://github.com/SpectrumQT/WWMI-Package) and copy its `Core/` folder into your `WWMI` directory, place character skins in `Mods/`, and keep **at most one copy** of `RabbitFX` under `Mods/` if your skins require it.

---

## Texture Pack & Graphics Settings Compatibility

| Resource Pack / Setting | Status | Notes & Recommendations |
|---|---|---|
| **`UHD` Pack (`-krqlv=uhd`)** | **Fully Verified (Recommended)** | Built-in Route 3 structural + 5-patch visual fingerprint online matcher (plus upstream `seed/` acceleration) resolves texture hash drift caused by 4K UHD re-encoding automatically. |
| **`HD` Pack (Standard High-Def)** | **Native Support (Recommended)** | Most community WWMI mod hashes are authored against `HD` and hit the fast exact-hash path immediately. |
| **`SD` Pack / Low Graphics Settings** | **Partial / Limited** | (1) Community mods target **LOD0 high-poly meshes**—keep **LOD Bias / Character Detail** on **High** so models do not revert to vanilla LOD1/LOD2; (2) Auxiliary unmatched textures downscaled below `256×256` safely fall back to vanilla textures. |
| **Graphics API** | **DirectX 12 Required** | Never pass `-dx11` or check "Launch with DX11" (use vanilla XXMI Launcher for DX11). |

### Verified Mod Compatibility Scope

> **Disclaimer**: Our runtime architecture is completely generic (zero per-character or per-skin hardcoding), **however we have only directly tested the community skins listed below** (under DX12 + Hardware Ray Tracing + 4K UHD `-krqlv=uhd`, alongside a single shared `RabbitFX` where required, on both `WWMI 1.1.0` and `WWMI 1.2.2`). We **cannot guarantee** that every community mod works out of the box.

#### Reference Tested Environment (Linux / Proton)

- **OS / GPU / Driver**: CachyOS Linux, NVIDIA GeForce RTX 5090 D (driver `620.70.06`)
- **Compatibility Layer**: `dwproton-11.0-16-x86_64` + `Steam Linux Runtime 4.0`
- **Reference Launch Command / Steam Launch Options** (DX12 + Hardware RT + 4K UHD + UTF-8 mod path guard + optional NVIDIA DLSS Multi-Frame Generation):
  ```bash
  SteamOS=1 PROTON_ENABLE_NGX_UPDATER=1 \
  DXVK_NVAPI_DRS_SETTINGS="NGX_DLSS_FG_OVERRIDE=on,NGX_DLSSG_MODE=dynamic,NGX_DLSSG_DYNAMIC_TARGET_FRAME_RATE=240,NGX_DLSSG_DYNAMIC_MULTI_FRAME_COUNT_MAX=5" \
  WINEDLLOVERRIDES="dxgi=n,b" HOST_LC_ALL=C.UTF-8 \
  LD_PRELOAD="/path/to/WWMI-DX12-RT/lib/wine-filename-utf8.so" \
  %command% -krqlv=uhd
  ```
  - Required for mod loading: `WINEDLLOVERRIDES="dxgi=n,b"` (plus `HOST_LC_ALL=C.UTF-8` and `LD_PRELOAD=...` if you have non-ASCII mod folder names).
  - Optional (NVIDIA RTX 50 / DLSS-G users): `PROTON_ENABLE_NGX_UPDATER=1` and `DXVK_NVAPI_DRS_SETTINGS=...` (omit if not using dynamic multi-frame generation; omit `-krqlv=uhd` if your client uses the HD pack).

Verified working in our test environment:
- `Chisa Shrine Maiden NSFW`
- `Hiyuki - Mist of Morning Glory`
- `hsin-Moonover Fox`
- `Iuno Sapphire Moon`
- `Lily Lewd Saint`
- `Lynae - Origin Toggle`
- `shorekeeper OL NSFW switch`
- `Suoming-Crimson Nocturne`
- `洛瑟菈-暗夜魅影-序列045`
- `清宵-夏日清风-序列058`
- `穗穗-白澜穗光-序列053`

If a mod does not render properly or fails to load in your setup, please open a **[GitHub Issue](https://github.com/Bud-Open/wwmi-dx12-rt/issues)** and include:
1. The **mod name** and **download link**;
2. Your latest `wwmi_dx12_candidate-*.log` (from `<ExternalFolder>/runtime/logs/`);
3. Your graphics settings (**Ray Tracing ON/OFF**, **DLSS/FSR Frame Generation**, `UHD`/`HD`/`SD` pack, and any other active ReShade add-ons).

---

## Quick Start & Uninstall

Download the latest portable archive from **[GitHub Releases](https://github.com/Bud-Open/wwmi-dx12-rt/releases)**:
- Windows: `WWMI-DX12-RT-win64.zip`
- Linux / SteamOS / Steam Deck: `WWMI-DX12-RT-linux.tar.gz`

### 1. Windows (Official Kuro Launcher & Steam)

1. Extract `WWMI-DX12-RT-win64.zip` to a fixed folder **outside** the game directory (for example `D:\Tools\WWMI-DX12-RT`—never extract directly into `Client\Binaries\Win64`).
2. Double-click **`WWMI-DX12-RT-Setup.exe`** (single-file, zero-dependency native Win32 GUI):
   - Auto-detects *Wuthering Waves* across the official Kuro Launcher and Steam libraries (click **Browse** for custom paths).
   - Auto-links `%AppData%\XXMI Launcher\WWMI` (or click **Init Skeleton** and copy `WWMI-Package`'s `Core/` inside).
   - Click **Install & Enable**, then close the window once it reports success.
3. **Play**: Open the official **Kuro Launcher** or **Steam** as usual and start the game in **DirectX 12** mode.
4. **Upgrade / Uninstall**:
   - **Upgrade**: Extract the new ZIP over your existing external folder (keeping `runtime\install-ledger.json`) and click **Install & Enable**, or click **Uninstall / Restore** in the old folder before switching directories.
   - **Uninstall**: Click **Uninstall / Restore** at any time to restore a clean `Win64` directory. Uninstall before moving to a different game install folder.

### 2. Linux / SteamOS / Steam Deck (Proton)

1. Extract `WWMI-DX12-RT-linux.tar.gz` to a fixed folder (for example `~/Tools/WWMI-DX12-RT`):
   ```bash
   # GUI wizard (auto-detects kdialog / zenity)
   ./wwmi-dx12-setup gui

   # Optional: create a desktop application menu entry
   ./wwmi-dx12-setup desktop-entry

   # Or CLI mode: doctor / install / uninstall
   ./wwmi-dx12-setup doctor
   ./wwmi-dx12-setup install --init-missing-wwmi
   ./wwmi-dx12-setup uninstall
   ```
2. **One-Time Steam Launch Options** (**Steam → Wuthering Waves → Properties → Launch Options**):
   - ASCII mod folder names:
     ```text
     WINEDLLOVERRIDES="dxgi=n,b" %command%
     ```
   - Non-ASCII / Chinese mod folder names: run `./wwmi-dx12-setup gui --proton-utf8-guard` (or pass `--proton-utf8-guard` to `install`) and copy the printed string into Steam Launch Options. Modern Proton uses [`HOST_LC_ALL=C.UTF-8`](https://github.com/ValveSoftware/Proton#runtime-config-options); if enabling the optional `LD_PRELOAD` shim, keep both extract and install paths free of spaces and colons.
3. **Flatpak Steam Note**: Run `./wwmi-dx12-setup doctor`—if your external folder or `WWMI` tree lies outside Flatpak Steam's sandbox, `doctor` prints the exact `flatpak override --user --filesystem=...` command to grant access.
4. **Play**: Click **Play** directly in Steam.

---

## Troubleshooting & FAQ

- **Mods are not loading (game shows vanilla models)**
  - Confirm *Wuthering Waves* is running in **DirectX 12** mode (not DX11).
  - On Linux/Proton, verify `WINEDLLOVERRIDES="dxgi=n,b" %command%` is set in Steam Launch Options.
  - Press **`Home`** in-game and confirm the status panel shows green `DX12 Ray Tracing + Hybrid Raster active` and `Active mods > 0`.
  - Confirm `Core/WWMI/WuWa-Model-Importer.ini` exists inside your `WWMI` directory.
- **Installer refuses to overwrite existing `dxgi.dll` or `ReShade.ini` / Coexisting with RenoDX or other ReShade add-ons**
  - This safety check prevents clobbering third-party ReShade/OptiScaler/XXMI files or orphaned mounts. Back up and move the pre-existing `Win64/dxgi.dll` and `Win64/ReShade.ini` (or run **Uninstall** from your previous WWMI-DX12-RT folder), then re-run **Install & Enable**.
  - **Important**: WWMI-DX12-RT requires its paired `dxgi.dll` (`ReShade64-game-forward.dll`) to expose DX12+RT hooks—never swap it for stock ReShade `dxgi.dll`.
  - To load third-party ReShade add-ons (such as RenoDX) alongside WWMI-DX12-RT, either copy their `*.addon64` files into `<ExternalFolder>/reshade-addons/`, or set `LoadGameDirAddons=1` (or `ExtraAddonPaths=<dir1>,<dir2>`) under `[ADDON]` in `<ExternalFolder>/ReShade.ini` (UTF-8). Custom settings in `<ExternalFolder>/ReShade.ini` are preserved on upgrades. If mods fail during gameplay, temporarily disable third-party add-ons to rule out hook conflicts.
- **WWMI Core version compatibility (`1.1.0` & `1.2.2+`)**
  - Both legacy `WWMI 1.1.0` and newer `WWMI 1.2.2+` core packages work out of the box. The DX11-only `XXMI DLL v1.3.3+` dHash banner is suppressed automatically (not applicable to DX12-RT), while genuine mod version warnings remain dismissible with **`X`**.
- **Legacy Hash Mod textures mis-match on 4K UHD (`-krqlv=uhd`)**
  - Route 3 automatically learns and caches matches online; for stubborn legacy mods, use the offline hash-free migrator and seed tools documented in [`tools/README.md`](./tools/README.md).
- **Where are runtime logs and caches stored?**
  - Inside `<ExternalFolder>/runtime/logs/` and `<ExternalFolder>/runtime/cache/`. Nothing is written inside `Client/Binaries/Win64/` during gameplay.

---

## Repository Layout & Module Licensing

```text
wwmi-dx12-rt/
├── source/
│   ├── host/            # [BSD-3-Clause] Derived from ReShade: D3D12 AS/slot tracking, copy-merge, compute forwarding & sorted add-on loader
│   ├── main/            # [GPL-3.0] Derived from wwmi-dx12 + 3Dmigoto: external BasePath bootstrap, RT hybrid raster gate, Route 3 visual matcher
│   ├── client/          # [GPL-3.0] zz_wwmi_rt_pack DXR + hybrid raster consumer backend
│   ├── include/         # [GPL-3.0] Decoupled pure-POD ABI headers between host, main addon, and RT consumer
│   ├── shaders/rt_pack/ # [GPL-3.0] HLSL compute shaders for DXR skinning, SBT patching, and runtime discovery
│   └── linux/           # [GPL-3.0] Linux Wine UTF-8 filename preload shim (wine-filename-utf8.c)
├── tools/               # [GPL-3.0] Ledger installer core, Win32 GUI setup, Route 3 seed & hash-free migration tools
├── scripts/             # Build, packaging, upstream dependency bootstrap (bootstrap_deps.py), and Linux setup/dev scripts
├── tests/               # Unit tests & Wine Win32 installer smoke test
├── payload/licenses/    # Complete verbatim upstream and third-party license texts bundled with releases
├── LICENSE              # Repository-wide license (GNU GPL-3.0)
└── THIRD-PARTY-NOTICES.md # Full upstream provenance, change summary, and community credits
```

---

## Building from Source & Testing

This repository includes a self-contained `CMake + Ninja` build chain with zero proprietary `.obj` dependencies, supporting native Windows builds (CI-verified in [`.github/workflows/ci-build.yml`](./.github/workflows/ci-build.yml)) and Linux cross-compilation:

- **Prerequisites**:
  - **Windows (Native)**: Visual Studio / MSVC Build Tools (with `ClangCL`), `CMake >= 3.23`, `Ninja`, `Python >= 3.10`.
  - **Linux (Cross-Compile)**: `clang-cl`, `lld-link`, `llvm-rc`, xwin Windows SDK (`cmake/toolchains/linux-clang-cl-xwin.cmake`), `CMake >= 3.23`, `Ninja`, `Python >= 3.10`.

```bash
# 1. Fetch pinned public upstream repositories into gitignored third_party/
#    (crosire/reshade @ 05c95f4a, lekarsenten/wwmi-dx12 @ ec837ef8, DirectXShaderCompiler v1.8.2502)
python3 scripts/bootstrap_deps.py

# 2. Build all targets from source:
#    - ReShade64-game-forward.dll (paired D3D12/DXGI host)
#    - wwmi_dx12.addon64          (main addon + migoto_engine; zero Nektra linkage)
#    - zz_wwmi_rt_pack.addon64    (DXR + hybrid raster consumer backend)
#    - WWMI-DX12-RT-Setup.exe     (zero-dependency native Win32 installer)
#    - lib/wine-filename-utf8.so  (Linux Wine UTF-8 preload shim; built automatically on Linux hosts)
python3 scripts/build.py

# 3. Run unit tests (including host C++ Route 3 solver test) and assemble portable archives
python3 -m unittest discover -s tests -v
python3 scripts/package_release.py                  # auto: win64+linux on Linux, win64 on Windows
python3 scripts/package_release.py --target win64   # or --target linux / --target all
```

### Shader & Reproducibility Notes

- `zz_wwmi_rt_pack` ships only the 10 compute shaders built from `source/shaders/rt_pack/*.hlsl` (compiled via pinned DXC on Windows; matching `.cso` used for Linux cross-builds). The 3 upstream base shaders (`Bc7Half`, `BufCopy`, `IbVbHash`) and PCRE2 10.30 (BSD) have their full matching source code in the pinned upstream checkout.
- No game-captured DXIL shaders or pre-baked character profiles are redistributed. Native RT geometry discovery runs automatically on startup with external caching; optional upstream `wwmi_dx12_seed` files can accelerate initial texture matching, while a clean build without seeds also works out of the box via Route 3 online learning. Whenever insufficient evidence exists for a replacement path, native ray-tracing dispatch is preserved untouched.
- GLAD compiles from checked-in generated sources by default (no Jinja2 required); pass `-DWWMI_REGENERATE_GLAD=ON` with Jinja2 installed to regenerate.
- Every release archive embeds `BUILD-PROVENANCE.json` verified against `build-manifest.json`, recording the exact repository commit, clean-tree status, and pinned upstream revisions.

---

## License & Credits

- **Overall License**: This repository, all project-authored modules, and combined binary releases are licensed under the **[GNU General Public License v3.0 (`GPL-3.0`)](./LICENSE)**.
- **Upstream Component Licenses**:
  - `source/main/` is derived from [`lekarsenten/wwmi-dx12`](https://github.com/lekarsenten/wwmi-dx12) (`2026.10.4`) and [`SpectrumQT/3Dmigoto`](https://github.com/SpectrumQT/3Dmigoto) (XXMI branch) under `GPL-3.0`.
  - `source/host/` is derived from [`crosire/reshade`](https://github.com/crosire/reshade) under `BSD-3-Clause` (with project extensions under `GPL-3.0`).
  - All other modules added by this project (`source/client/`, `source/include/`, `source/shaders/`, `source/linux/`, `tools/`, `tests/`) are licensed under **`GPL-3.0`**; third-party files (`crc32c`, `bcdec`, ReShade headers, etc.) retain their compatible upstream licenses.
- **Full Notices & Acknowledgments**: See **[`THIRD-PARTY-NOTICES.md`](./THIRD-PARTY-NOTICES.md)**. Special thanks to **lekarsenten** (`wwmi-dx12`), **Patrick Mours / crosire** (`ReShade`), **SpectrumQT & SinsOfSeven** (`WWMI` / `XXMI`), **Chiri / Bo3b / DarkStarSword / SilentNightSound / Nurarihyon** (`3Dmigoto`), and **caverabbit** (`RabbitFX`).
