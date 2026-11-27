# WWMI-DX12-RT

[English](./README.md) | **简体中文**

[![License: GPL-3.0](https://img.shields.io/badge/License-GPL--3.0-blue.svg)](./LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Windows%20%7C%20Linux%20%2F%20SteamOS-informational)](#快速安装与卸载指南)
[![Graphics API](https://img.shields.io/badge/API-DirectX%2012%20%2B%20DXR-success)](#画质档位与游戏设置兼容性说明)

面向《鸣潮》（Wuthering Waves）**原生 DirectX 12 + 硬件光线追踪（DXR）+ DLSS 多帧生成 + 4K UHD（`-krqlv=uhd`）** 环境的开源 WWMI 运行时与跨平台外部挂载工具链。

本项目基于 [`lekarsenten/wwmi-dx12`](https://github.com/lekarsenten/wwmi-dx12)（`GPL-3.0`）与 [`crosire/reshade`](https://github.com/crosire/reshade)（`BSD-3-Clause`）扩展，提供 **硬件光线追踪消费后端（`zz_wwmi_rt_pack`）、Route 3 结构+视觉 UHD 免哈希贴图适配、跨文件系统确定性排序加载与 2 文件外部挂载工具链**。

> **说明**：本仓库仅提供开源运行时、光追消费后端与跨平台安装器，**不包含**任何游戏原始资产或角色皮肤 Mod。

---

## 核心特性

- **原生 DX12 + 硬件光追 + 多帧生成全兼容**：开启游戏内光线追踪反射 / 全局光照与 DLSS 帧生成时，角色皮肤 Mod 同时正常参与混合光栅渲染与 DXR 光追反射，不降画质、不退回 DX11。
- **兼容现有 WWMI 社区 Mod 生态**：直接读取标准 `WWMI` 目录（`d3dx.ini`、`Core/WWMI/`、`Mods/RabbitFX/`、`Mods/*/` 及 `XXMI Launcher` / `IMM` 嵌套管理的皮肤包），无需转换 Mod 文件。
- **原生贴图过滤（A23，默认开启）**：DX12 光栅和 DX12 光追模式均支持 3Dmigoto v1.3.3 的 `[Filter]` / `dHash` 匹配，以及 WWMI 1.2.2 Core 的 `Helpers.ini` 中的 `ps->srv_*` 查询。
- **光追模式全功能实时控制**：
  - **`F10`**：游戏内在线热重载全部 Mod 与 `.ini` 配置（无需重启游戏）。
  - **`F6`**：实时开启 / 关闭 Mod 渲染（同时覆盖混合光栅与 DXR 光追反射，秒切对比原版）。
  - **`Home`**：呼出 ReShade 叠加面板，直接显示绿色的 `DX12 Ray Tracing + Hybrid Raster active` 光追状态与活跃 Mod 计数。
- **「安装一次，原习惯无感直启」**：
  - **Windows（库洛官方启动器 / Steam）**：运行一次原生 Win32 配置器完成挂载后，后续直接打开**官方启动器**或 **Steam** 点击「开始游戏」即可。
  - **Linux / SteamOS / Steam Deck（Proton）**：挂载后在 Steam 启动选项一次性填入 `WINEDLLOVERRIDES="dxgi=n,b" %command%`，后续直接从 Steam 启动。
- **极低侵入与零运行时污染（2-File External `BasePath` Mount）**：
  - 安装时仅向游戏 `Client/Binaries/Win64/` 写入 **`2` 个引导文件**（`dxgi.dll` 与 2 行路径跳转 `ReShade.ini`），内置进程名白名单（仅对 `Client-Win64-Shipping.exe` 激活，不干扰启动器进程）。
  - 所有核心插件（`.addon64`）、光追着色器、`wwmi_dx12.ini`、缓存、持久开关（`runtime/d3dx_user.ini`）与轮转日志均**隔离在游戏目录外**，运行期对游戏目录 **0 字节写入**。
  - **非破坏性账本卸载**：随时点击【一键卸载还原】（或执行 `./wwmi-dx12-setup uninstall`），仅暂存账本确认归属的 2 个文件，拒绝误覆写第三方 `dxgi.dll` / `ReShade.ini`。
- **自包含全开源构建链**：基于 `CMake + Ninja` 支持 Windows 原生编译（已配置 GitHub Actions CI）与 Linux 交叉编译，移除闭源 Nektra 依赖（零 `NktHookLib64.lib` 链接），随包附 `BUILD-PROVENANCE.json` 与完整第三方许可。

---

## 使用前准备

1. 确保《鸣潮》以 **DirectX 12 模式**运行（切勿启用 DX11 启动选项）。
2. 准备好 **WWMI Mod 目录**：
   - 若已安装 `XXMI Launcher`，安装器会自动探测现有 `WWMI` 目录；
   - 若为全新环境，可在安装器内点击 **【新建骨架】**，再将官方 [`WWMI-Package`](https://github.com/SpectrumQT/WWMI-Package) 的 `Core/` 目录放入其中，把皮肤放入 `Mods/`，需要 RabbitFX 的皮肤在 `Mods/` 下**仅保留唯一一份** `RabbitFX`。

---

## 画质档位与游戏设置兼容性说明

| 画质资源包 / 设置项 | 兼容状态 | 说明与建议 |
|---|---|---|
| **`UHD`（超清资源包，`-krqlv=uhd`）** | **已全量真机验证（推荐）** | 内置 Route 3 结构与 5-Patch 视觉指纹后台在线自学习适配（并支持上游 `seed/` 加速），自动克服 4K 超清包贴图重编码导致的哈希漂移。 |
| **`HD`（标准高清资源包）** | **原生兼容（推荐）** | 社区绝大多数 WWMI Mod 贴图哈希提取自 `HD` 包，直接第一层精确哈希命中。 |
| **`SD`（极速精简包）/ 极低画质设置** | **存在边界限制** | ① 社区 Mod 仅针对 **LOD0 高模网格**制作，请将「角色精细度 / LOD」保持在 **高**（避免切换至 LOD1/LOD2 变回原版模型）；② 若 `SD` 包将未命中哈希的辅助贴图压缩至 `< 256×256`，局部小贴图将安全回退为原版材质。 |
| **渲染 API 模式** | **必须为 DirectX 12** | 请勿勾选「使用 DX11 启动」或传入 `-dx11`（DX11 模式请继续使用原版 XXMI Launcher）。 |

### 已实测皮肤清单与兼容范围说明

> **兼容范围声明**：本项目采用 100% 通用渲染架构（无任何单角色/单皮肤硬编码），但受限于人力，**目前我们仅实机测试过以下皮肤**（测试环境：DX12 + 硬件光线追踪 + 4K UHD `-krqlv=uhd`，按需共用唯一一份 `RabbitFX`，覆盖 `WWMI 1.1.0` 与 `WWMI 1.2.2`），**无法保证社区所有皮肤均 100% 正常**。

#### 实测参考环境与启动命令行（Linux / Proton）

- **系统 / 显卡 / 驱动**：CachyOS Linux，NVIDIA GeForce RTX 5090 D（驱动 `620.70.06`）
- **兼容层**：`dwproton-11.0-16-x86_64` + `Steam Linux Runtime 4.0`
- **参考启动命令行 / Steam 启动选项**（DX12 + 硬件光追 + 4K UHD + 中文路径 UTF-8 护航 + 可选 NVIDIA DLSS 多帧生成）：
  ```bash
  SteamOS=1 PROTON_ENABLE_NGX_UPDATER=1 \
  DXVK_NVAPI_DRS_SETTINGS="NGX_DLSS_FG_OVERRIDE=on,NGX_DLSSG_MODE=dynamic,NGX_DLSSG_DYNAMIC_TARGET_FRAME_RATE=240,NGX_DLSSG_DYNAMIC_MULTI_FRAME_COUNT_MAX=5" \
  WINEDLLOVERRIDES="dxgi=n,b" HOST_LC_ALL=C.UTF-8 \
  LD_PRELOAD="/path/to/WWMI-DX12-RT/lib/wine-filename-utf8.so" \
  %command% -krqlv=uhd
  ```
  - 加载 Mod 必需：`WINEDLLOVERRIDES="dxgi=n,b"`（含中文/非 ASCII 文件夹名皮肤时加上 `HOST_LC_ALL=C.UTF-8` 与 `LD_PRELOAD=...`）。
  - 可选项：`PROTON_ENABLE_NGX_UPDATER=1` 与 `DXVK_NVAPI_DRS_SETTINGS=...` 为 NVIDIA 动态多帧生成参考配置（按需删改）；若客户端安装的是 HD 包则去掉 `-krqlv=uhd`。

当前已实测运行正常的皮肤：
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

如果遇到其他皮肤无法生效、贴图异常或渲染错乱，欢迎提交 **[GitHub Issue](https://github.com/Bud-Open/wwmi-dx12-rt/issues)** 并附上：
1. **异常皮肤的名称**与**下载地址**；
2. 最新一份运行日志 `wwmi_dx12_candidate-*.log`（位于 `<外部目录>/runtime/logs/`）；
3. 游戏内画质设置（**硬件光追开/关**、**DLSS/FSR 帧生成**、`UHD`/`HD`/`SD` 资源包，以及是否同时加载了其他 ReShade 插件）。

---

## 快速安装与卸载指南

前往 **[GitHub Releases](https://github.com/Bud-Open/wwmi-dx12-rt/releases)** 下载对应平台的便携压缩包：
- Windows：`WWMI-DX12-RT-win64.zip`
- Linux / SteamOS / Steam Deck：`WWMI-DX12-RT-linux.tar.gz`

### 一、Windows 用户（官方启动器 / Steam 通用）

1. 将 `WWMI-DX12-RT-win64.zip` 解压到**游戏目录之外**的任意固定文件夹（例如 `D:\Tools\WWMI-DX12-RT`，切勿解压进游戏 `Win64` 目录）。
2. 双击运行 **`WWMI-DX12-RT-Setup.exe`**（单文件零依赖原生 Win32 图形配置器）：
   - 自动探测库洛官方启动器与 Steam 跨盘库中的《鸣潮》目录（自定义路径点「浏览」选择即可）；
   - 自动关联现有 `%AppData%\XXMI Launcher\WWMI`（若无 XXMI，点 **【新建骨架】** 并放入 `WWMI-Package` 的 `Core/`）；
   - 点击 **【一键安装启用】**，提示成功后关闭即可。
3. **开始游玩**：按日常习惯打开 **库洛官方启动器** 或 **Steam**，以 **DirectX 12 模式**点击「开始游戏」。
4. **升级与彻底卸载**：
   - **升级新版**：直接覆盖原外部目录（保留 `runtime\install-ledger.json`）后重跑【一键安装启用】，或先在旧目录点击 **【一键卸载还原】** 再迁移新目录。
   - **彻底卸载**：随时点击 **【一键卸载还原】** 即可安全移除 `Win64` 下的 2 个引导文件恢复纯净状态；切换游戏安装目录前请先卸载旧挂载。

### 二、Linux / SteamOS / Steam Deck 用户（Proton）

1. 解压 `WWMI-DX12-RT-linux.tar.gz` 到固定目录（例如 `~/Tools/WWMI-DX12-RT`）：
   ```bash
   # 图形向导模式（自动适配 kdialog / zenity）
   ./wwmi-dx12-setup gui

   # 可选：创建桌面应用菜单快捷方式
   ./wwmi-dx12-setup desktop-entry

   # 或命令行模式：环境自检 / 安装 / 卸载
   ./wwmi-dx12-setup doctor
   ./wwmi-dx12-setup install --init-missing-wwmi
   ./wwmi-dx12-setup uninstall
   ```
2. **首次配置 Steam 启动选项**（**Steam → Wuthering Waves → 属性 → 启动选项**）：
   - 纯英文 Mod 目录名：
     ```text
     WINEDLLOVERRIDES="dxgi=n,b" %command%
     ```
   - 含中文文件夹名的 Mod：运行 `./wwmi-dx12-setup gui --proton-utf8-guard`（或 `install --proton-utf8-guard`），将输出复制到 Steam 启动选项。现代 Proton 使用 [`HOST_LC_ALL=C.UTF-8`](https://github.com/ValveSoftware/Proton#runtime-config-options)；若启用可选 `LD_PRELOAD` 垫片，解压和安装路径均不可含空格或冒号。
3. **Flatpak Steam 提示**：运行 `./wwmi-dx12-setup doctor`，若外部挂载目录或 `WWMI` 位于 Flatpak 沙箱不可见路径，工具会自动给出精确的 `flatpak override --user --filesystem=...` 授权命令。
4. **开始游玩**：直接在 Steam 点击「开始游戏」即可。

---

## 常见问题排查（FAQ）

- **进游戏后 Mod 没有生效（仍是原版皮肤）**
  - 确认游戏运行在 **DirectX 12** 模式（非 DX11）；
  - Linux/Proton 用户确认 Steam 启动选项已填入 `WINEDLLOVERRIDES="dxgi=n,b" %command%`；
  - 游戏内按 **`Home`**，确认状态面板显示绿色 `DX12 Ray Tracing + Hybrid Raster active` 且 `Active mods > 0`；
  - 确认 `WWMI` 目录下存在 `Core/WWMI/WuWa-Model-Importer.ini`。
- **安装器拒绝覆盖现有 `dxgi.dll` 或 `ReShade.ini` / 与 RenoDX 等第三方 ReShade 插件共存**
  - 这是防冲突保护机制，防止覆盖第三方 ReShade/OptiScaler/XXMI 或旧挂载残留。请先备份移走原 `Win64/dxgi.dll` 与 `Win64/ReShade.ini`（或在旧外部目录执行一次【卸载还原】）后再安装。
  - **注意**：WWMI-DX12-RT 必须使用本包配对的 `dxgi.dll`（`ReShade64-game-forward.dll`）才能提供 DX12+RT 挂钩，切勿替换为原版 ReShade `dxgi.dll`。
  - 如需同时加载 RenoDX 等第三方 ReShade 插件，可将对应 `*.addon64` 复制到 `<外部目录>/reshade-addons/`，或在 `<外部目录>/ReShade.ini`（UTF-8）的 `[ADDON]` 下设置 `LoadGameDirAddons=1`（或 `ExtraAddonPaths=<目录1>,<目录2>`）；后续升级重装会自动保留你的外部 `ReShade.ini` 自定义项。若游戏内 Mod 异常，请先临时停用第三方插件排查冲突。
- **WWMI 核心版本兼容（`1.1.0` 与 `1.2.2+`）**
  - 同时兼容旧版 `WWMI 1.1.0` 与新版 `WWMI 1.2.2+` 核心包：DX12 原生贴图过滤无需 DX11 XXMI DLL `v1.3.3+`，仍可通过 **`X`** 键正常关闭真实 Mod 提示框。
- **部分老旧 Hash Mod 在 4K UHD（`-krqlv=uhd`）下贴图错位**
  - 运行时 Route 3 会自动学习并缓存；若需批量转为跨画质、跨补丁的 Hash-Free 格式，可使用 [`tools/README.md`](./tools/README.md) 中的离线迁移与种子管理工具。
- **日志与缓存在哪里？**
  - 均位于 `<外部目录>/runtime/logs/` 与 `<外部目录>/runtime/cache/`，游戏 `Client/Binaries/Win64/` 运行期零写入。

---

## 仓库目录结构与模块归属

```text
wwmi-dx12-rt/
├── source/
│   ├── host/            # [BSD-3-Clause] 衍生自 ReShade：D3D12 加速结构槽位追踪、拷贝合并、Compute 转发与排序 Addon 加载宿主
│   ├── main/            # [GPL-3.0] 衍生自 wwmi-dx12 + 3Dmigoto：外部 BasePath 自举、光追混合光栅门控、Route 3 视觉自适配
│   ├── client/          # [GPL-3.0] zz_wwmi_rt_pack 硬件光追（DXR）与混合光栅消费后端
│   ├── include/         # [GPL-3.0] 宿主/主插件/光追后端解耦纯 POD ABI 契约头文件
│   ├── shaders/rt_pack/ # [GPL-3.0] DXR 光追材质/蒙皮/SBT/自动发现 HLSL 着色器源码
│   └── linux/           # [GPL-3.0] Linux Wine 中文路径 UTF-8 预加载垫片（wine-filename-utf8.c）
├── tools/               # [GPL-3.0] 跨平台账本安装核心、Win32 GUI 安装器、离线免哈希迁移与种子工具
├── scripts/             # 构建、打包、上游依赖拉取（bootstrap_deps.py）与 Linux 配置/开发脚本
├── tests/               # 自动化单元测试套件与 Wine Win32 安装器冒烟测试
├── payload/licenses/    # 随包分发的完整上游与第三方组件许可证原文
├── LICENSE              # 仓库整体开源许可证（GNU GPL-3.0）
└── THIRD-PARTY-NOTICES.md # 完整上游血统溯源、修改清单与社区致谢
```

---

## 开发者从源码构建与测试

本项目提供独立自包含的 `CMake + Ninja` 源码构建链（零私有 `.obj` 依赖，支持 Windows 原生构建 [`.github/workflows/ci-build.yml`](./.github/workflows/ci-build.yml) 与 Linux 交叉编译）：

- **构建前置环境**：
  - **Windows 原生**：Visual Studio / MSVC Build Tools（含 `ClangCL`）、`CMake >= 3.23`、`Ninja`、`Python >= 3.10`。
  - **Linux 交叉编译**：`clang-cl`、`lld-link`、`llvm-rc`、xwin Windows SDK（`cmake/toolchains/linux-clang-cl-xwin.cmake`）、`CMake >= 3.23`、`Ninja`、`Python >= 3.10`。

```bash
# 1. 自动拉取锁定版本的公开上游基座至 gitignored 的 third_party/
#    (crosire/reshade @ 05c95f4a, lekarsenten/wwmi-dx12 @ ec837ef8, DirectXShaderCompiler v1.8.2502)
python3 scripts/bootstrap_deps.py

# 2. 编译全部核心目标：
#    - ReShade64-game-forward.dll (配对 D3D12/DXGI 宿主)
#    - wwmi_dx12.addon64          (主插件 + migoto_engine；零 Nektra 链接)
#    - zz_wwmi_rt_pack.addon64    (硬件光追 DXR 与混合光栅消费后端)
#    - WWMI-DX12-RT-Setup.exe     (零依赖原生 Win32 安装配置器)
#    - lib/wine-filename-utf8.so  (Linux Wine UTF-8 路径垫片，Linux 主机自动构建)
python3 scripts/build.py

# 3. 运行单元测试套件（含 C++ Route 3 求解器单测）并组装便携发行包
python3 -m unittest discover -s tests -v
python3 scripts/package_release.py                  # 默认：Linux 产出 win64+linux，Windows 产出 win64
python3 scripts/package_release.py --target win64   # 或 --target linux / --target all
```

### 着色器与可复现说明

- `zz_wwmi_rt_pack` 只分发本仓 `source/shaders/rt_pack/*.hlsl` 对应的 10 个计算着色器（Windows 用锁定 DXC 重编，Linux 交叉构建使用仓内对应 `.cso`）；3 个基础主插件 `.cso`（`Bc7Half`/`BufCopy`/`IbVbHash`）与 PCRE2 10.30（BSD）在锁定上游中均包含完整源码。
- 不分发游戏捕获的 DXIL 或预置角色档案。首局启动自动发现并维护外部缓存；上游可选 `wwmi_dx12_seed` 可加速初次贴图匹配，干净克隆无种子也可通过 Route 3 在线学习正常生效；证据不足时始终保留原生光追分派。
- GLAD 默认直接编译仓内生成代码（无需 Jinja2），需重新生成时传入 `-DWWMI_REGENERATE_GLAD=ON`。
- 发行包根目录均内嵌经 `build-manifest.json` 哈希校验的 `BUILD-PROVENANCE.json`，记录精确提交哈希、干净工作树状态与锁定上游版本。

---

## 开源许可证与致谢（License & Credits）

- **整体许可证**：本仓库、全部本项目新增模块及联合二进制发行包统一遵循 **[GNU General Public License v3.0 (`GPL-3.0`)](./LICENSE)**。
- **上游与第三方组件许可**：
  - `source/main/` 基于 [`lekarsenten/wwmi-dx12`](https://github.com/lekarsenten/wwmi-dx12)（`2026.10.4`）及 [`SpectrumQT/3Dmigoto`](https://github.com/SpectrumQT/3Dmigoto)（XXMI 分支）修改与扩展，遵循 `GPL-3.0`；
  - `source/host/` 基于 [`crosire/reshade`](https://github.com/crosire/reshade) 修改，上游原文件保留 `BSD-3-Clause`，本项目新增扩展遵循 `GPL-3.0`；
  - 本项目新增的全部模块（`source/client/`、`source/include/`、`source/shaders/`、`source/linux/`、`tools/`、`tests/`）均遵循 **`GPL-3.0`**；内含第三方文件（`crc32c`、`bcdec`、ReShade 头文件等）保留各自兼容的上游许可证。
- **完整第三方声明与致谢**：详见 **[`THIRD-PARTY-NOTICES.md`](./THIRD-PARTY-NOTICES.md)**。衷心感谢 **lekarsenten**（`wwmi-dx12`）、**Patrick Mours / crosire**（`ReShade`）、**SpectrumQT & SinsOfSeven**（`WWMI` / `XXMI`）、**Chiri / Bo3b / DarkStarSword / SilentNightSound / Nurarihyon**（`3Dmigoto`）以及 **caverabbit**（`RabbitFX`）的卓越开源贡献。
