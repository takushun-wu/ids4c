# ids4c Python API（中文）

[English](python-api.md)

本文只说明 Python binding 的使用方式。构建、Python 版本匹配、运行时数据库准备和数据导入来源请参阅根目录 README；查询语法请参阅 [query-syntax-zh.md](query-syntax-zh.md)。

## 导入模块

```python
import ids4c
print(ids4c.__version__)    # 0.4.0
print(ids4c.__copyright__)
```

主要接口：

- parse()：解析 IDS 或查询表达式；
- equal()：严格比较两个 IDS 树；
- IDSNode：拥有自身数据的 IDS 节点；
- Database：执行 IDS 查询和数据库操作；
- QueryOptions、MatchDetail、MatchPath、QueryResult：查询选项和结果对象。

## IDS 解析

### parse(text)

```python
node = ids4c.parse("⿰亻言")

print(node.kind)                              # pattern
print(node.text)                              # ⿰亻言
print(node.idc)                               # ⿰
print([child.text for child in node.children])  # ['亻', '言']
```

也可以解析项目支持的查询表达式：

```python
ids4c.parse("⿰贝⬚")
ids4c.parse("<search=长,<residue=31->>")
ids4c.parse("⿰<any=巾,忄>亡")
ids4c.parse("⿱<var=a>隹")
```

非法输入会抛出 ValueError，错误信息可能包含 Unicode code point 位置：

```python
try:
    ids4c.parse("⿰一")
except ValueError as error:
    print(error)
```

### equal(left, right)

这是严格的 IDS 树比较，不执行 IWDS、HVExtract 或其他模糊等价处理：

```python
assert ids4c.equal(
    ids4c.parse("⿰亻言"),
    ids4c.parse("⿰亻言"),
)
```

## IDSNode

常用属性：

| 属性 | 说明 |
| --- | --- |
| kind | stroke、ideograph、pattern、search、variable 等节点类型 |
| text | 节点序列化后的 IDS/查询表达式 |
| children | 结构节点子节点，或搜索项 |
| idc | 结构符，如 ⿰、⿱ |
| idc_type | C++ IDCtype 数值 |
| terms | 搜索表达式条件 |
| except_terms | 排除条件 |
| mode | all、any 或 except |
| unique_separator | 根部唯一化元数据，如 `{字}`，没有时为空字符串 |
| is_alternative_definition | 导入的定义是否来自另类定义组 |

字形节点还提供 glyph、codepoint、variation_selector、suffix、abstract_name。

`is_alternative_definition` 是来源元数据，不是部件或排除条件。直接解析的节点默认为 False；通过 `database.raw_ids()` 可以检查白式导入后保留的标记。严格树比较不区分定义组。重叠修饰符保留在 `text` 中，当前未向 Python 单独暴露类型化的 `OverlapMatrix` 属性。

笔画节点提供 stroke_data、break_positions、cross_data、enclosed。查询参数节点提供 param、stroke_minimum 和 stroke_maximum；变量节点提供 variable_name。

```python
glyph = ids4c.parse("嬴")
print(glyph.glyph, hex(glyph.codepoint), glyph.suffix)

pattern = ids4c.parse("⿰亻言")
print([child.text for child in pattern.children])
```

### 复制和替换子节点

```python
query = ids4c.parse("⿰亻言")
copy = query.clone()
changed = query.with_children(ids4c.parse("⿰亻信").children)

assert ids4c.equal(copy, query)
print(changed.text)  # ⿰亻信
```

返回的节点拥有独立数据，不依赖原节点生命周期。

## Database

### 创建和读取 IDS

```python
database = ids4c.Database("yibai0")

print(database.name)
print(database.is_empty())
print(database.last_error)

raw_entries = database.raw_ids("嬴")
cached_entries = database.ids("嬴")

for entry in raw_entries:
    print(entry.text)
```

raw_ids() 返回原始 IDS；ids() 返回数据库导入后的缓存/预处理 IDS。两者都返回 list[IDSNode]。

### HV 展开和等效查询

```python
alternatives = database.hv_extract(
    ids4c.parse("乾"),
    first_layer=True,
    preserve_ambiguous=False,
    maximum=64,
)

for alternative in alternatives:
    print(alternative.text)

print(database.equivalent_queries(ids4c.parse("⿰贝⬚")))
```

hv_extract() 可能返回多个结果，maximum 用于限制展开数量。

### 基础匹配

```python
query = ids4c.parse("⿰贝⬚")
matches = database.match(query)

print(matches[:5])
```

match() 只返回匹配到的字形字符串。

## 查询选项

```python
options = ids4c.QueryOptions()
options.track_match_paths = True
options.overlap_match_mode = ids4c.OverlapMatchMode.IGNORE
options.strict_enclosure_match = False
options.filter.ignore_overlay_structure = True
options.filter.result_filter = ids4c.ResultFilter.IGNORE_OTHER_LOCALES_KEEP_IVS
options.filter.glyph_domain = ids4c.GlyphDomain.UNICODE
options.filter.unicode_blocks = [ids4c.UnicodeBlock.CJK_BASIC]

matches = database.match(query, options)
```

QueryOptions：

- filter：FilterOptions；
- overlap_match_mode：`OverlapMatchMode.IGNORE`（默认）或 `CONSTRAINED`；后者检查重叠矩阵及查询通配符；
- strict_enclosure_match：默认为 False。True 关闭包围近似匹配，保留严格重排，不关闭 IWDS；
- track_match_paths：是否计算详细匹配路径。只需要字形列表时可以设为 False。

FilterOptions：

- ignore_overlay_structure：忽略匹配路径中使用 ⿻ 的结果；
- result_filter：ALL、IGNORE_LC_SUFFIX、IGNORE_OTHER_LOCALES_BASE_ONLY 或 IGNORE_OTHER_LOCALES_KEEP_IVS；
- glyph_domain：ALL、UNICODE、PRIVATE 或 ABSTRACT；
- unicode_blocks：一个或多个 UnicodeBlock；
- custom_ranges：已注册的自定义范围名称。
- locale_suffix_fallback_order：优先级组列表，例如 `["C=G", ".", "H", "T"]`；`=` 表示同级，`.` 表示无后缀。不能直接赋值 CLI 字符串 `C=G>.>H>T`。

当前 Python binding 可以选择已由宿主程序注册的范围，但尚未暴露注册自定义范围的接口。
`IGNORE_OTHER_LOCALES_BASE_ONLY` 将同一基码位的 IVS 变体合并；`IGNORE_OTHER_LOCALES_KEEP_IVS` 将不同的「基码位 + 异体字选择符」（包括无选择符）视为独立结果。严格 locale 筛选只在本次实际匹配的结果中选择。可用 `options.filter.locale_suffix_fallback_order` 指定后缀优先级；旧枚举值 `IGNORE_OTHER_LOCALES` 已移除。
可用枚举包括 ResultFilter、GlyphDomain、UnicodeBlock、IWDSUnificationLevel 和 DatabaseFormat。

没有无后缀定义时，明确设置后缀顺序可以让结果筛选保留优先级最高的可用组中实际命中的地区字形；若无后缀定义存在但不匹配，不以其他地区的命中替代它。请区分结果筛选与 `config.fuzzy_match.locale_suffix_fallback_order`：后者用于查找部件定义。

限定重叠状态的示例：

```python
options = ids4c.QueryOptions()
options.overlap_match_mode = ids4c.OverlapMatchMode.CONSTRAINED
query = ids4c.parse("⿻[?,x]丨日")
matches = database.match(query, options)
```

此处 `?` 对应一整行，包括空行。完整通配符及包围规则见 [query-syntax-zh.md](query-syntax-zh.md)。C++ 的 `IDSQueryProfile` 尚未暴露给 Python；分段诊断可以使用 CLI `--profile`。

## 详细结果

### match_detailed(query, options=None)

```python
details = database.match_detailed(query, options)

for detail in details:
    print(detail.glyph)
    print(detail.matched_ids)
    print(detail.raw_ids)
    print(detail.match_kind)
    print(detail.match_source)
    print(detail.preprocess_rules)
```

MatchDetail 主要属性：

| 属性 | 说明 |
| --- | --- |
| glyph | 匹配到的字形 |
| matched_ids | 实际成功匹配的 IDS |
| raw_ids | 数据库原始 IDS |
| match_kind | 精确、结构或部件搜索等类别 |
| match_source | 原始 IDS、HV 缓存等来源 |
| preprocess_rules | 生效的预处理规则名称列表 |
| match_paths | MatchPath 列表 |

MatchPath 属性：

- kind：路径类型；
- index：适用时的索引，否则为 None；
- query_expression_index：命中的等效查询编号，否则为 None；
- query_path：查询表达式中的路径；
- path：候选 IDS 中的路径。

路径使用紧凑格式，例如 /child[0:1]。不需要路径时，将 track_match_paths 设为 False。

### query(query, options=None)

合并返回等效查询和详细结果：

```python
result = database.query(query, options)

print(result.equivalent_syntax)
for detail in result.matches:
    print(detail.glyph, detail.match_kind)
```

QueryResult 提供：

- equivalent_syntax：预处理后的等效查询；
- matches：MatchDetail 列表；
- to_dict()：转换为适合 JSON 序列化的字典。

MatchDetail 和 MatchPath 也提供 to_dict()。

## 配置

```python
config = database.config
config.fuzzy_match.unification_level = (
    ids4c.IWDSUnificationLevel.SOURCE_CODE_SEPARATION
)
config.fuzzy_match.default_region = "G"
config.fuzzy_match.locale_suffix_fallback_order = ["C=G", ".", "H", "T"]
config.fuzzy_match.stroke_neutral_composition = True
config.fuzzy_match.exclude_non_equivalent_same_ids = True
config.misc.enable_cache = True
config.misc.sym_fallback = True
config.misc.suffix_is_alt_form = False

database.config = config
```

DatabaseConfig 包含 fuzzy_match 和 misc 两组配置。修改配置会影响该对象之后的操作。
`default_region` 提供回退后缀；`fuzzy_match.locale_suffix_fallback_order` 在精确字形定义不存在时提供查找部件定义的回退顺序，已有的明确后缀定义优先。结果筛选需另行设置 `options.filter.locale_suffix_fallback_order`，使用相同的列表格式。
`exclude_non_equivalent_same_ids` 默认为 True：明确以 `{字}` 区分的字形不会仅因 IDS 相同就互认。设为 False 可允许这类同 IDS 匹配。

## 导入数据

```python
database.import_file(
    "path/to/ids_lv0.txt",
    ids4c.DatabaseFormat.YIBAI,
)

database.import_private_file(
    "path/to/private.dat",
    ids4c.DatabaseFormat.DEFAULT,
    replace=True,
)
```

- import_file()：导入 IDS 数据；
- import_private_file()：追加或替换私有扩展数据；
- replace=True：替换已有私有扩展数据。

导入失败时抛出 RuntimeError。错误信息和结构化报告可通过 database.last_error 和 database.last_import_report 获取。

`last_import_report` 是一个字典，主要字段包括：`input_lines`、`data_lines`、`source_expressions`、`accepted_expressions`、`rejected_expressions`、`query_cache_entries`、`rebuilt_cache_glyphs`、`cache_truncations` 和 `issues`。`issues` 中的每项包含 `line`、`ids_index`、`character_index`、`glyph`、`expression` 和 `message`，适合显示导入错误位置。

导入完成后，数据库会从原始 IDS 生成 HV 缓存、笔画中性组合缓存和一级部件倒排索引。`raw_ids()` 返回原始定义，`ids()` 返回查询缓存；缺失或旧版索引会在加载数据库时重建或迁移。需要从原始定义重新生成派生数据时，建议重新导入数据库或使用 C++ API 的 `RebuildQueryCache()`。C++ 导入 API 支持阶段回调，当前 Python binding 尚未暴露该回调。

0.4.0 的白式数据库要求 schema 8；旧 schema 必须从源文件重新导入，不能仅靠重建缓存恢复另类定义组信息。完整导入基础库会替换数据库内容，随后需要重新导入私有扩展，因此请保留两类源文件。升级流程见 [cli-zh.md](cli-zh.md#升级至-040)。

## 对象和线程

- IDSNode 和数据库返回对象拥有自己的数据；
- 不建议在没有外部同步的情况下同时对一个 Database 对象进行查询和导入；
- 多线程使用时，可以为每个线程创建独立的 Database 对象，或由调用方提供锁；
- Python binding 适合作为 IDS 树处理基础层，例如实现 IDS 到笔画序列的展开。

当前 binding 以基础能力和稳定的数据结构为目标，尚未承诺覆盖所有 C++ 内部接口。
