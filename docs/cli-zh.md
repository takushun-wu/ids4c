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

## 查询参数

| 参数 | 短参数 | 说明 |
| --- | --- | --- |
| --database NAME | -d | SQLite 数据库名称，不含 .sqlite |
| --query IDS | -q | 执行 IDS 查询 |
| --inspect GLYPH | | 查看一个字形的原始 IDS、缓存 IDS、同 IDS 字形和包含它的字形 |
| --explain | -e | 输出详细匹配信息 |
| --show-ids | | --explain 的别名 |
| --no-match-paths | | 关闭详细匹配路径计算 |

GLYPH 可以是实际字符、U+XXXX、0xXXXX 或抽象字形名。

## IWDS、区域和筛选

| 参数 | 可选值/格式 | 说明 |
| --- | --- | --- |
| --unification-level | none、srcseparation、lv1、lv2 | IWDS 模糊统合等级 |
| --default-region | 区域字符串 | 精确部件字形不存在时使用的默认区域/后缀 |
| --locale-suffix-order | `>`、`=` 分隔的后缀列表 | 指定地区后缀的回退顺序；`.` 表示无后缀 |
| --result-filter | all、ignore-lc-suffix、ignore-other-locales | 结果后缀和区域筛选 |
| --glyph-domain | all、unicode、private、abstract | 字形域筛选 |
| --unicode-block | 逗号分隔 | Unicode 区块筛选 |
| --ignore-overlay | 无值 | 忽略匹配路径使用 ⿻ 的结果 |

--unicode-block 支持 all、cjk、basic、ext-a 至 ext-j、compatibility、radicals、strokes、private-bmp、private-plane15、private-plane16、abstract 和 other。

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

导入失败时，CLI 会输出错误信息以及输入行、IDS 序号和字符位置等诊断信息。

### 导入后的缓存

IDS 导入成功后会从原始表达式生成 HV 查询缓存和笔画中性组合缓存。原始 IDS 与派生缓存都写入同一个 SQLite 数据库；缓存不应手工编辑。私有库追加或重新导入时，受影响字形的缓存会增量更新。

如果原始数据库来自新的 `ids_lv0.txt` 或者缓存内容与输入文件不一致，可以重新导入数据库；C++ API 还提供 `RebuildQueryCache()` 供上层程序从当前原始 IDS 重建缓存。导入报告会给出接受的表达式数、缓存条目数、缓存截断数以及错误列表。
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
  --result-filter ignore-other-locales `
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

## 运行目录

数据库名称 NAME 对应当前工作目录下的：

```text
db/NAME.sqlite
```

运行 CLI 前，请确认当前目录下存在目标数据库。Windows 下如果程序依赖的 MinGW/GTKmm DLL 不在系统路径中，还需要先将对应运行时目录加入 PATH。
