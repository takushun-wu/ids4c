# 变更记录 / Changelog

## v0.3.0

### 新增 / Added

- 为原始 IDS、HV 查询缓存和笔画中性组合缓存建立一级部件倒排索引，并持久化到 SQLite；旧版或缺失的索引可在加载时迁移或重建。
- 导入基础库及私有库时提供读取、缓存构建、笔画计算和保存等阶段通知；CLI 与 GUI 显示当前阶段。
- 增加「筛除其他 locale，按基码位合并 IVS」与「筛除其他 locale，保留不同 IVS」两种结果筛选模式。
- 增加关闭 same-IDS 唯一化排除的选项；默认仍尊重 `{字}` 标记。

- Added a persistent first-level component index for raw IDS, HV query cache, and stroke-neutral compositions. Missing or older SQLite indexes are migrated or rebuilt on load.
- Added import-stage notifications for base and private libraries, displayed by the CLI and GUI.
- Added two strict locale filters: collapse IVS variants by base code point, or keep distinct variation selectors.
- Added an option to disable same-IDS uniqueness exclusion; `{glyph}` markers remain respected by default.

### 改进 / Changed

- `<search=...>` 使用保守的索引候选筛选、普通部件最小笔画数剪枝，以及多条件预检查；简单 `<any=...>` 不再拆成多次完整查询。
- 将替换与减法查询的等效展开移到查询预处理阶段；一级倒排表对同一部件与字形只存一行，并用位掩码记录来源。
- 完整重新导入数据库时不再预先加载将被覆盖的旧数据库。

- Improved `<search=...>` with conservative indexed candidates, per-term minimum-stroke pruning, and a necessary-condition check for multiple terms. Simple `<any=...>` terms no longer trigger separate full queries.
- Moved replacement and subtraction expansion into query preprocessing. The component index stores one row per component/glyph pair with a source bitmask.
- Full database imports no longer load the database they are about to replace.

### 修复 / Fixed

- HV 递归匹配保留唯一化来源，避免 `{曰}` 等部件被错误视作同 IDS 的其他汉字。
- 单笔画搜索可匹配 `#(...)` 中的笔画 token（包括负笔画），但重复搜索项不能重复占用同一 token。
- 严格 locale 筛选只根据本次实际命中的结果选取地区字形，并按所选模式正确处理 IVS。

- Preserved uniqueness origins during recursive HV matching, preventing `{曰}` and similar components from matching unrelated same-IDS glyphs.
- Single-stroke searches can match stroke tokens in `#(...)`, including negative strokes; repeated terms cannot reuse one token.
- Strict locale filtering now selects regional glyphs from the current match set and handles IVS according to the selected mode.

### 兼容性 / Compatibility

- CLI 的 `--result-filter ignore-other-locales` 已替换为 `ignore-other-locales-base-only` 和 `ignore-other-locales-keep-ivs`；Python `ResultFilter` 枚举相应更名。

- Replaced CLI `--result-filter ignore-other-locales` with `ignore-other-locales-base-only` and `ignore-other-locales-keep-ivs`; the Python `ResultFilter` enum names changed accordingly.

## v0.2.1

### 改进 / Changed

- `<var=...>` 在 ▥/▤ 的直接子项中可绑定连续的同向节点，并与可展开为相同节点序列的部件匹配。
- 匹配路径现在可标出此类变量对应的 HV 节点范围。

- `<var=...>` can bind a contiguous sequence of nodes in a ▥/▤ arrangement and match a component that expands to the same sequence.
- Match paths now identify the HV node ranges used by these variable bindings.

### 修复 / Fixed

- 修复候选节点不足时，多变量排列匹配可能发生的越界问题。

- Prevented out-of-bounds access when an arrangement has too few candidate nodes for multiple variables.

## v0.2.0

### 新增 / Added

- 补充 HV 范围标记、笔画中性组合缓存和地区后缀回退。

- Added HV range markers, stroke-neutral combination cache, and regional suffix fallback.


## v0.1.0

- 首次发布。

- First release.
