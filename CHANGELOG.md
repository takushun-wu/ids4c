# 变更记录 / Changelog

## v0.4.0

### 新增 / Added

- 支持 `⿻` 的类型化重叠矩阵，保留至多两个矩阵、切片端点、空行及矩阵顺序；增加 `ignore`（默认）与 `constrained` 两种匹配模式。
- 增加重叠查询通配符：切片端点 `*`、行级 `?` / `*`、单个矩阵 `**`、整个修饰符 `***`；全包围、半包围定位支持 `[*]`。行级 `?` 也可匹配空行，查询通配符不能导入数据库。
- 增加角部包围与左右、上下组合的近似匹配，默认启用；GUI、CLI `--strict-enclosure-match` 和 Python `QueryOptions.strict_enclosure_match` 可关闭近似规则，同时保留严格的包围结构重排。
- CLI 增加 `--profile`，将预处理、same-IDS、候选索引、扫描、筛选及详细信息生成的分段计时和候选计数输出到 stderr；C++ API 提供 `IDSQueryProfile`。
- 统一 GUI、CLI 和 Python 模块的版本号与版权信息；Windows EXE/PYD 增加版本资源，CLI 提供 `--version`，Python 提供 `__version__` 与 `__copyright__`。

- Added typed overlap matrices for `⿻`, preserving up to two matrices, slice endpoints, empty rows, and matrix order. Matching supports `ignore` (default) and `constrained` modes.
- Added overlap query wildcards: `*` for slice endpoints, row-level `?` / `*`, `**` for one matrix, and `***` for the entire modifier. Enclosure positions accept `[*]`; row `?` can also match an empty row. Query wildcards cannot be imported as database definitions.
- Added approximate matching between corner enclosures and horizontal/vertical compositions, enabled by default. The GUI, CLI `--strict-enclosure-match`, and Python `QueryOptions.strict_enclosure_match` can disable approximations while retaining exact enclosure reassociation.
- Added CLI `--profile` for stage timings and candidate counts on stderr, covering preprocessing, same-IDS expansion, indexing, scanning, filtering, and detailed output construction. The C++ API exposes `IDSQueryProfile`.
- Unified version and copyright information across the GUI, CLI, and Python module. Windows EXE/PYD files include version resources; the CLI exposes `--version`, and Python exposes `__version__` and `__copyright__`.

### 改进 / Changed

- 扩展一级部件倒排索引的候选筛选范围，并保留无法安全索引时的扫描回退；补充 HV 重组及原始 IDS 递归匹配的覆盖。
- 完善包围结构的双向外部重排与嵌套包围匹配；非零定位数字限制不适用的重排，严格模式不启用内部近似规则。
- 按白式 IDS §4.2 拆分逗号并列的变体标识，增加默认定义组与另类定义组的元数据标记，并加强 §5 笔画语法校验。
- SQLite IDS 数据库格式升级至 schema 8，使用 `ids_entries.is_alternative` 保存另类定义标记；Python IDS 节点增加 `is_alternative_definition` 属性。
- 「忽略其他地区」在字形没有无后缀定义时，按照配置的 `localeSuffixFallbackOrder` 选择回退地区；保留不同 IVS 与合并 IVS 两种筛选语义。

- Expanded first-level index candidate filtering while retaining scan fallback where indexing is unsafe, with additional coverage for HV compositions and recursive raw IDS matching.
- Improved bidirectional external enclosure reassociation and nested-enclosure matching. Nonzero position constraints restrict incompatible transformations; strict mode excludes internal approximations.
- Split comma-separated variant identifiers according to YiBai IDS §4.2, added metadata distinguishing default and alternative definition groups, and strengthened §5 stroke syntax validation.
- Updated the SQLite IDS schema to version 8, storing alternative-definition metadata in `ids_entries.is_alternative`. Python IDS nodes expose `is_alternative_definition`.
- Locale filtering now follows the configured `localeSuffixFallbackOrder` when no unsuffixed definition exists, retaining both IVS-preserving and IVS-collapsing modes.

### 修复 / Fixed

- 修复部分 HV 拆分及包围结构的漏匹配，包括 `誓` 的多部件搜索和跨层包围组合；补充相关回归测试。
- 修复仅有带地区后缀定义的字形被地区筛选完全排除的问题；若无后缀定义存在但不满足查询，不以其他地区的命中替代它。

- Fixed missed matches in selected HV decompositions and enclosure layouts, including multi-component searches for `誓` and enclosure compositions across tree levels, with regression tests.
- Fixed locale filtering dropping glyphs with only localized definitions. If an unsuffixed definition exists but does not match, a localized hit does not replace it.

### 升级说明 / Upgrade Notes

- **旧版白式 SQLite 数据库需要从源 IDS 文件重新导入**：旧格式无法可靠恢复另类定义组信息，加载时会明确提示重新导入。请保留基础库及私有库的源文件。
- C++ 最低要求仍为 C++17；重叠矩阵及查询选项相关 API 已扩展，请重新编译依赖库和 Python 模块。PYD 仍需使用与目标 Python ABI 和架构匹配的构建。
- 默认近似匹配可能增加结果；需要严格结构查询时启用 `--strict-enclosure-match` 或对应 GUI / API 选项。切片与穿插矩阵之间的语义归一化尚未实现。

- **Reimport older YiBai SQLite databases from their source IDS files.** Older schemas cannot reliably recover alternative-definition metadata and now report an explicit reimport requirement. Keep source files for both base and private libraries.
- C++17 remains the minimum requirement. Overlap and query-option APIs have been extended; rebuild dependent libraries and Python modules. PYD builds must still match the target Python ABI and architecture.
- Approximate matching can add results by default. Enable `--strict-enclosure-match` or the corresponding GUI/API option for strict structural queries. Semantic normalization between slice notation and crossing matrices is not yet implemented.

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

- 最低编译要求改为 C++17，CMake 现在显式使用 C++17 标准编译项目目标。
- CLI 的 `--result-filter ignore-other-locales` 已替换为 `ignore-other-locales-base-only` 和 `ignore-other-locales-keep-ivs`；Python `ResultFilter` 枚举相应更名。

- The minimum compiler requirement is now C++17; CMake explicitly builds project targets in C++17 mode.
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
