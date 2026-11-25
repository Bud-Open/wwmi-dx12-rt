# `tools/` — Route 3 跨画质与跨版本通用贴图工具链

本目录承载 **Route 3（全画质通吃 + 传统 Hash Mod 跨补丁复活）** 的开源工具链，100% 通用、零角色/哈希硬编码、仅依赖 Python 3 标准库（可选调用内置 MIT/Unlicense `third_party/bcdec.h` 启用 5-patch 视觉指纹解歧义）。

## 1. Layer 2：`texlook_tool.py`（模块化种子管理与审计）

支持解析、合并、去重、按角色导出 `.texlook` / `tex_look.bin` 与 `.lodcache`，以及消费 `TEX-UHD-AUDIT` 日志生成组件槽位映射报告。

```bash
# 查看 .texlook 或 .lodcache 统计信息（种子文件为可选加速输入，可从运行期缓存导出）
python3 tools/texlook_tool.py inspect /path/to/your_seed.texlook
python3 tools/texlook_tool.py inspect /path/to/your_seed.lodcache

# 合并多个增量种子并剥离会话 verdict / kMixed 冲突
python3 tools/texlook_tool.py merge --out merged.texlook --drop-verdicts --drop-mixed base.texlook patch.texlook
python3 tools/texlook_tool.py merge --out merged.lodcache --exact-only base.lodcache patch.lodcache

# 按角色/Mod 导出紧凑增量种子包（<BasePath> 为外部挂载目录，例如 ~/.local/share/wwmi-dx12-rt）
python3 tools/texlook_tool.py export-role \
  --texlook <BasePath>/runtime/cache/tex_look.bin \
  --lodcache <BasePath>/runtime/cache/lod_tex_cache.lodcache \
  --mod-ini /path/to/Mod/mod.ini \
  --audit-log <BasePath>/runtime/logs/wwmi_dx12_candidate.log \
  --out role_seed.texlook \
  --out-lodcache role_seed.lodcache \
  --exact-only

# 从 TEX-UHD-AUDIT + tex_look.bin 生成组件槽位状态报告
python3 tools/texlook_tool.py report \
  --audit-log <BasePath>/runtime/logs/wwmi_dx12_candidate.log \
  --texlook <BasePath>/runtime/cache/tex_look.bin \
  --lodcache <BasePath>/runtime/cache/lod_tex_cache.lodcache \
  --mod-ini /path/to/Mod/mod.ini
```

> 导出的 `.texlook` / `.lodcache` 增量种子可直接放入 `<BasePath>/runtime/seed/`（或 `<BasePath>/seed/`），`wwmi_dx12` 启动时按文件名词典序非破坏性叠加加载，不重命名源文件、不破坏现有缓存。

## 2. Layer 3：`migrate_hash_free.py`（离线 `Hash -> Hash-Free` 无损迁移）

读取任意传统 Hash Mod 的 `mod.ini` + `.dds` 头部元信息 + 运行期 `TEX-UHD-AUDIT`（+ 可选 `tex_look.bin` / `lod_tex_cache.lodcache`），以无损方式输出全画质、跨补丁通用的 `ps-tN->Format` Hash-Free 方案（默认输出至独立 `--out-dir`，绝不修改原始 Mod 文件）。

```bash
# 全能力模式（锚点锁定 + 单义格式 + bcdec 5-patch 视觉指纹解歧义）
python3 tools/migrate_hash_free.py \
  --mod-dir "/path/to/ClassicMod" \
  --audit-log <BasePath>/runtime/logs/wwmi_dx12_candidate.log \
  --texlook <BasePath>/runtime/cache/tex_look.bin \
  --lodcache <BasePath>/runtime/cache/lod_tex_cache.lodcache \
  --out-dir /path/to/output_dir

# 纯结构模式（无种子、无 C 编译器环境；仅绑定锚点与严格单义槽）
python3 tools/migrate_hash_free.py \
  --mod-dir "/path/to/ClassicMod" \
  --audit-log <BasePath>/runtime/logs/wwmi_dx12_candidate.log \
  --no-visual \
  --out-dir /path/to/output_dir

# 编辑 output_dir/slot_mapping.json 后按用户微调重新生成 INI
python3 tools/migrate_hash_free.py \
  --mod-dir "/path/to/ClassicMod" \
  --audit-log <BasePath>/runtime/logs/wwmi_dx12_candidate.log \
  --mapping-override /path/to/output_dir/slot_mapping.json \
  --out-dir /path/to/output_dir_v2
```

输出产物：
- `migration_report.md`：每个组件与贴图槽位的匹配依据与置信度表；
- `slot_mapping.json`：可编辑的机器/人工映射清单；
- `hash_free_snippet.ini`：纯增量 Hash-Free 补丁片段；
- `mod_hash_free.ini`：完整无损合成的 Hash-Free `mod.ini`。

## 3. 自动化测试

```bash
python3 -m unittest discover -s tests -v
```
