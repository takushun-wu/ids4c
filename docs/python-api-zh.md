# ids4c Python API（中文）

[English](python-api.md)

本文只说明 Python binding 的使用方式。构建、Python 版本匹配、运行时数据库准备和数据导入来源请参阅根目录 README；查询语法请参阅 [query-syntax-zh.md](query-syntax-zh.md)。

## 导入模块

```python
import ids4c
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

字形节点还提供 glyph、codepoint、variation_selector、suffix、abstract_name。

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
options.filter.ignore_overlay_structure = True
options.filter.result_filter = ids4c.ResultFilter.IGNORE_OTHER_LOCALES
options.filter.glyph_domain = ids4c.GlyphDomain.UNICODE
options.filter.unicode_blocks = [ids4c.UnicodeBlock.CJK_BASIC]

matches = database.match(query, options)
```

QueryOptions：

- filter：FilterOptions；
- track_match_paths：是否计算详细匹配路径。只需要字形列表时可以设为 False。

FilterOptions：

- ignore_overlay_structure：忽略匹配路径中使用 ⿻ 的结果；
- result_filter：ALL、IGNORE_LC_SUFFIX 或 IGNORE_OTHER_LOCALES；
- glyph_domain：ALL、UNICODE、PRIVATE 或 ABSTRACT；
- unicode_blocks：一个或多个 UnicodeBlock；
- custom_ranges：已注册的自定义范围名称。

可用枚举包括 ResultFilter、GlyphDomain、UnicodeBlock、IWDSUnificationLevel 和 DatabaseFormat。

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
config.misc.enable_cache = True
config.misc.sym_fallback = True
config.misc.suffix_is_alt_form = False

database.config = config
```

DatabaseConfig 包含 fuzzy_match 和 misc 两组配置。修改配置会影响该对象之后的操作。

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

## 对象和线程

- IDSNode 和数据库返回对象拥有自己的数据；
- 不建议在没有外部同步的情况下同时对一个 Database 对象进行查询和导入；
- 多线程使用时，可以为每个线程创建独立的 Database 对象，或由调用方提供锁；
- Python binding 适合作为 IDS 树处理基础层，例如实现 IDS 到笔画序列的展开。

当前 binding 以基础能力和稳定的数据结构为目标，尚未承诺覆盖所有 C++ 内部接口。
