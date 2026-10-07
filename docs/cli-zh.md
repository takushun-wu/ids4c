# ids4c CLI

[English](cli.md)

本文说明 ids4c-cli 的命令行参数。项目构建、运行时数据库位置和数据来源请参阅根目录 README。

## 基本格式

```text
ids4c-cli --database NAME --query IDS [options]
ids4c-cli --database NAME --inspect GLYPH
ids4c-cli --database NAME --import SOURCE [--format default|yibai]
ids4c-cli --database NAME --import-private SOURCE [--format default|yibai]
ids4c-cli --database NAME --reimport-private SOURCE [--format default|yibai]
ids4c-cli --import-iwds SOURCE.xml
```

查看全部参数：

```powershell
.\build\ids4c-cli.exe --help
```

一次调用只能执行一个主要操作。除 --import-iwds 外，主要操作都需要 --database。

`--version` 不打开数据库，直接输出版本、版权和许可证信息：

```powershell
.\build\ids4c-cli.exe --version
```

## 查询参数

| 参数 | 短参数 | 说明 |
| --- | --- | --- |
| --database NAME | -d | SQLite 数据库名称，不含 .sqlite |
| --query IDS | -q | 执行 IDS 查询 |
| --inspect GLYPH | | 查看一个字形的原始 IDS、缓存 IDS、同 IDS 字形和包含它的字形 |
| --explain | -e | 输出详细匹配信息 |
| --show-ids | | --explain 的别名 |
| --no-match-paths | | 关闭详细匹配路径计算 |
| --profile | | 向标准错误输出查询分段计时和候选计数 |

GLYPH 可以是实际字符、U+XXXX、0xXXXX 或抽象字形名。

## IWDS、区域和筛选

| 参数 | 可选值/格式 | 说明 |
| --- | --- | --- |
| --unification-level | none、srcseparation、lv1、lv2 | IWDS 模糊统合等级 |
| --default-region | 区域字符串 | 精确部件字形不存在时使用的默认区域/后缀 |
| --locale-suffix-order | `>`、`=` 分隔的后缀列表 | 指定地区后缀的回退顺序；`.` 表示无后缀 |
| --result-filter | all、ignore-lc-suffix、ignore-other-locales-base-only、ignore-other-locales-keep-ivs | 结果后缀和区域筛选 |
| --disable-same-ids-exclusion | 无值 | 允许被 `{字}` 唯一化标记区分的同 IDS 字形互相匹配 |
| --glyph-domain | all、unicode、private、abstract | 字形域筛选 |
| --unicode-block | 逗号分隔 | Unicode 区块筛选 |
| --ignore-overlay | 无值 | 忽略匹配路径使用 ⿻ 的结果 |
| --overlap-match-mode | ignore、constrained | 默认忽略重叠修饰符，或限定重叠矩阵状态 |
| --strict-enclosure-match | 无值 | 关闭包围结构近似匹配，保留严格重排 |

--unicode-block 支持 all、cjk、basic、ext-a 至 ext-j、compatibility、radicals、strokes、private-bmp、private-plane15、private-plane16、abstract 和 other。

`ignore-other-locales-base-only` 将同一基码位的所有 IVS 变体视为一个结果；`ignore-other-locales-keep-ivs` 将「基码位 + 异体字选择符」（包括没有选择符）分别视为独立结果。严格 locale 筛选只在本次查询实际命中的字形中选择，`--locale-suffix-order` 不会令未命中的基础字形参与筛选。旧值 `ignore-other-locales` 已不再接受。

明确设置顺序（例如 `C=G>.>H>T`）时，`=` 表示同级，`>` 表示回退优先级。字形完全没有无后缀定义时，可以保留优先级最高的可用组中实际命中的地区字形；若无后缀定义存在但不匹配，不以其他地区的命中替代它。

`--ignore-overlay` 是排除重叠命中；`--overlap-match-mode constrained` 则允许重叠结构，但检查其修饰符。通配符及包围规则见 [query-syntax-zh.md](query-syntax-zh.md)。`--strict-enclosure-match` 独立于 IWDS 等级，只关闭包围近似规则，不关闭 IWDS 模糊统合。

## 输出格式

| 参数 | 可选值 | 说明 |
| --- | --- | --- |
| --output-format | text、csv、tsv、json | 查询结果格式 |
| --json-unicode | raw、escaped | JSON 是否将非 ASCII 字符写成 Unicode 转义 |

CSV、TSV 和 JSON 会输出详细字段；文本模式默认输出字形列表，配合 --explain 可以输出 IDS、来源、规则和路径。

## 数据库导入

### 导入 YiBai IDS

推荐使用 YiBai 的 ids_lv0.txt 作为精确笔画匹配基础数据：

```powershell
New-Item -ItemType Directory -Force db
.\build\ids4c-cli.exe `
  --database yibai0 `
  --import path/to/ids_lv0.txt `
  --format yibai
```

--format yibai 也适用于同类的 ids_lv1.txt 和 ids_lv2.txt。

### 导入默认格式

```powershell
.\build\ids4c-cli.exe `
  --database custom `
  --import path/to/source.dat `
  --format default
```

### 导入 IWDS

```powershell
.\build\ids4c-cli.exe --import-iwds path/to/iwds.xml
```

该操作生成或更新运行时使用的 db/unifiable.sqlite。

### 导入私有扩展库

追加或更新私有 IDS：

```powershell
.\build\ids4c-cli.exe `
  --database yibai0 `
  --import-private path/to/private.dat `
  --format default
```

替换数据库中已有的私有 IDS：

```powershell
.\build\ids4c-cli.exe `
  --database yibai0 `
  --reimport-private path/to/private.dat `
  --format default
```

IDS 导入时，CLI 会向标准错误输出当前阶段（读取、HV 缓存、笔画中性缓存、部件索引、笔画数、保存和完成）。导入失败时，还会输出输入行、IDS 序号和字符位置等诊断信息。

### 导入后的缓存

IDS 导入成功后会从原始表达式生成 HV 查询缓存、笔画中性组合缓存和一级部件倒排索引。原始 IDS 与派生数据都写入同一个 SQLite 数据库；缓存和索引不应手工编辑。私有库追加或重新导入时，受影响字形的缓存和索引会更新。缺失或旧版部件索引会在加载数据库时重建或迁移。

如果原始数据库来自新的 `ids_lv0.txt` 或者缓存内容与输入文件不一致，可以重新导入数据库；C++ API 还提供 `RebuildQueryCache()` 供上层程序从当前原始 IDS 重建缓存。导入报告会给出接受的表达式数、缓存条目数、缓存截断数以及错误列表。

### 升级至 0.4.0

旧 schema 的白式数据库需要使用源文本通过 `--import ... --format yibai` 重新导入。Schema 8 保存默认/另类定义组信息，旧数据库无法可靠恢复这项元数据，因此仅重建缓存不够。完整导入基础库会替换数据库内容，随后需要从源文件重新导入私有扩展。升级前请保留这些源文件。

`(.,T)` 等逗号分隔的变体标识分别生成无后缀和 `T` 记录；另类定义以 `ids_entries.is_alternative` 保存。该标记记录来源组，不会使这些定义被查询排除。

## 查询示例

基本结构查询：

```powershell
.\build\ids4c-cli.exe `
  --database yibai0 `
  --query "⿰贝⬚"
```

IWDS lv2 详细查询：

```powershell
.\build\ids4c-cli.exe `
  --database yibai0 `
  --unification-level lv2 `
  --explain `
  --query "⿰⬚𠧒"
```

组合筛选：

```powershell
.\build\ids4c-cli.exe `
  --database yibai0 `
  --result-filter ignore-other-locales-keep-ivs `
  --glyph-domain unicode `
  --unicode-block "basic,ext-a" `
  --ignore-overlay `
  --query "⿰牛⬚"
```

JSON 输出：

```powershell
.\build\ids4c-cli.exe `
  --database yibai0 `
  --query "⿰贝⬚" `
  --output-format json `
  --json-unicode escaped
```

限定重叠状态的查询：

```powershell
.\build\ids4c-cli.exe --database yibai0 `
  --overlap-match-mode constrained --query "⿻[?,x]丨日"
```

### 查询分段计时

```powershell
.\build\ids4c-cli.exe --database yibai0 `
  --unification-level lv2 --profile --query "<search=日,欠>"
```

`[profile]` 行以毫秒记录预处理、same-IDS 展开、候选索引、扫描、结果筛选和详细信息构建的耗时。`index_used=A/B` 表示 B 次索引尝试中有 A 次采用候选筛选，并不表示所有查询都能用索引；不适用的情况会回退扫描。计时用于诊断，不是性能保证；`--no-match-paths` 可以减少详细结果的路径计算。计时写入标准错误，不改变标准输出的结果格式。

## 运行目录

数据库名称 NAME 对应当前工作目录下的：

```text
db/NAME.sqlite
```

运行 CLI 前，请确认当前目录下存在目标数据库。Windows 下如果程序依赖的 MinGW/GTKmm DLL 不在系统路径中，还需要先将对应运行时目录加入 PATH。
