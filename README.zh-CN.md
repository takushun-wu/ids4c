# ids4c

[English documentation](README.md)

`ids4c` 是一个面向汉字 IDS（Ideographic Description Sequence，表意文字描述序列）的 C++ 库和工具集，提供 IDS 解析、树操作、数据库导入、结构匹配、部件搜索以及 IWDS 模糊统合支持。

项目同时提供：

- `ids4c`：GTKmm 图形界面程序；
- `ids4c-cli`：命令行查询和数据库导入工具；
- `ids4c.pyd`：基于 pybind11 的 Python 扩展模块；
- C++ 静态库和公开头文件，便于其他程序集成。

项目源代码使用 Apache License 2.0，详见 [LICENSE](LICENSE)。上游 IDS、IWDS 数据和语法文档分别遵循各自项目的许可和使用条件，详见[数据来源与参考](#数据来源与参考)。

## 功能概览

### IDS 解析和树操作

- 解析普通 IDS 表达式、白式 IDS 扩展语法和查询表达式；
- 保留结构符、部件、笔画、后缀、变量和查询参数等节点信息；
- 支持 IDS 树的复制、子节点访问和结构相等判断；
- 支持根据 IDS 结构进行笔画数计算和相关缓存查询。

### 查询和匹配

- 精确 IDS 匹配和结构匹配；
- 部件搜索相关语句：`<search=...>`、`<any=...>` 和 `<except=...>`；
- 通配部件 `⬚`、变量 `<var=...>` 和 Unicode 转义 `\uXXXX` / `\UXXXXXXXX`；
- 笔画条件 `<stroke=...>` 与剩余笔画条件 `<residue=...>`；
- 支持 `~` 表示允许的笔画误差，例如 `<stroke=21~1>` 和 `<residue=12~2>`；
- HVExtract 多解缓存和同 IDS 表达式归并；
- U+1F504（🔄）查询替换表达式；
- 可选的 `⿻` 组合结构过滤；
- 可选的 Unicode 区块、私用区、抽象字形名和自定义字形范围筛选；
- 可输出详细匹配路径、查询表达式编号和预处理规则。

### IWDS 模糊统合

数据库查询可以选择以下 IWDS 统合等级：

- `none`：不进行 IWDS 模糊统合；
- `srcseparation`：Source Code Separation；
- `lv1`：在 Source Code Separation 基础上加入组件级统合；
- `lv2`：在更高等级上加入更多 IWDS 组件统合。

统合查询会在查询预处理阶段展开为等效表达式，然后交给 IDS 匹配层处理。查询结果可保留命中的等效表达式和路径信息，便于上层程序解释结果。

## 特化语法文档

查询表达式的完整语法说明已移至 [docs/query-syntax-zh.md](docs/query-syntax-zh.md)。该文档涵盖 <search>、<any>、<except>、笔画/余笔画条件、变量、Unicode 转义、U+1F504 替换查询以及 ▥、▤ 等结构扩展。

## 目录结构

```text
include/ids4c/       C++ 公共头文件
src/core/            IDS 树、解析和核心匹配逻辑
src/db/              SQLite 数据库、IDS 导入和 IWDS 导入
src/cli/             命令行程序
src/app/             GUI 程序入口
src/ui/              GTKmm 用户界面
src/python/          pybind11 Python 模块
docs/                API 文档
tests/               CTest 测试和测试数据
third_party/         项目直接使用的第三方源码
```

## 构建要求

基础构建要求：

- CMake 3.20 或更高版本；
- 支持 C++11 的编译器；
- SQLite 3；
- `pkg-config`；
- Boost，CLI 需要 `program_options`，GUI 需要 `filesystem` 和 `system`。

额外目标的依赖：

- GUI：GTKmm 3；
- Python binding：CPython 开发文件和 pybind11 的 CMake 配置包；
- CLI：Boost.Program_options。

在 Windows 上，建议使用 MSYS2 的 MinGW 环境，并确保 Python、pybind11、SQLite、GTKmm、Boost 与 CMake 使用同一套目标架构和工具链。Python 扩展是原生 CPython 模块，面向不同 Python 版本、架构或工具链时通常需要重新编译。

## 构建

### 构建 GUI、CLI 和 Python binding

Python binding 默认开启。若本机已安装与编译器匹配的 Python 和 pybind11，可以使用：

```powershell
cmake -S . -B build `
  -DIDS4C_BUILD_GUI=ON `
  -DIDS4C_BUILD_CLI=ON `
  -DIDS4C_BUILD_PYTHON=ON `
  -DCMAKE_BUILD_TYPE=Release

cmake --build build --config Release
```

如果 CMake 无法自动找到 Python 或 pybind11，可以显式指定：

```powershell
cmake -S . -B build -G "MinGW Makefiles" `
  -DIDS4C_BUILD_GUI=ON `
  -DIDS4C_BUILD_CLI=ON `
  -DIDS4C_BUILD_PYTHON=ON `
  -DPython3_EXECUTABLE=E:/msys64/mingw64/bin/python.exe `
  -DPython3_ROOT_DIR=E:/msys64/mingw64 `
  -Dpybind11_DIR=E:/msys64/mingw64/lib/cmake/pybind11 `
  -DCMAKE_BUILD_TYPE=Release

cmake --build build --config Release
```

### 不构建 Python binding

没有 Python 开发环境或暂时不需要 Python 模块时，可关闭该目标：

```powershell
cmake -S . -B build `
  -DIDS4C_BUILD_GUI=ON `
  -DIDS4C_BUILD_CLI=ON `
  -DIDS4C_BUILD_PYTHON=OFF `
  -DCMAKE_BUILD_TYPE=Release

cmake --build build --config Release
```

主要 CMake 选项：

| 选项 | 默认值 | 作用 |
| --- | --- | --- |
| `IDS4C_BUILD_GUI` | `ON` | 构建 GTKmm GUI |
| `IDS4C_BUILD_CLI` | `ON` | 构建 CLI |
| `IDS4C_BUILD_PYTHON` | `ON` | 构建 pybind11 模块 |
| `IDS4C_ENABLE_LTO` | `OFF` | 在 Release/RelWithDebInfo 下启用 LTO/IPO |

当前发布代码不携带完整运行时数据库。请按照下一节导入上游 IDS 和 IWDS 数据来准备运行时数据库。

构建成功后，Windows 下的主要产物通常位于 `build/`：

```text
ids4c.exe       GUI
ids4c-cli.exe   CLI
ids4c.pyd       Python 扩展模块
```

Python 模块不是独立程序。使用它时，应让 Python 能找到 `ids4c.pyd`，并让运行时 DLL 位于 `PATH` 中。例如在 MSYS2 MinGW 环境中：

```powershell
$env:PATH = "E:/msys64/mingw64/bin;$env:PATH"
$env:PYTHONPATH = "${PWD}/build"
python -c "import ids4c; print(ids4c.parse('⿰亻言').text)"
```

## 安装

构建完成后，可以使用以下命令安装启用的目标、头文件和文档：

```powershell
cmake --install build --prefix install
```

## 运行时数据库

`ids4c` 使用 SQLite 数据库。运行程序时，数据库名称 `NAME` 对应当前工作目录下的：

```text
db/NAME.sqlite
```

例如，`--database yibai0` 会打开 `db/yibai0.sqlite`。如果存在 `db/unifiable.sqlite`，程序还会读取其中的 IWDS 统合数据。

数据库文件由 IDS 源数据导入生成，不要求把上游原始文本直接复制到程序目录。推荐使用 YiBai 的 `ids_lv0.txt` 作为精确笔画匹配的基础数据：

```powershell
New-Item -ItemType Directory -Force db
.\build\ids4c-cli.exe `
  --database yibai0 `
  --import path/to/ids_lv0.txt `
  --format yibai
```

导入 IWDS XML：

```powershell
.\build\ids4c-cli.exe --import-iwds path/to/iwds.xml
```

导入失败时，CLI 会输出错误信息以及输入行、IDS 序号和字符位置等诊断信息。私有扩展库可以追加或重新导入：

```powershell
.\build\ids4c-cli.exe `
  --database yibai0 `
  --import-private path/to/private.dat `
  --format default

.\build\ids4c-cli.exe `
  --database yibai0 `
  --reimport-private path/to/private.dat `
  --format default
```

数据库导入格式包括：

- `default`：项目默认 IDS 输入格式；
- `yibai`：YiBai `ids_lv0.txt`、`ids_lv1.txt`、`ids_lv2.txt` 一类的三列数据格式。

上游数据库的原始格式、主 IDS 与替代 IDS 的含义，请以 YiBai 项目说明为准。

## CLI 使用

CLI 参数、数据导入、筛选器、输出格式和示例见 [docs/cli-zh.md](docs/cli-zh.md)。

最小查询示例：

```powershell
.\build\ids4c-cli.exe --database yibai0 --query "⿰贝⬚"
```
## Python API

Python API 说明见 [中文文档](docs/python-api-zh.md) 和 [English documentation](docs/python-api.md)。最小示例：

```python
import ids4c

query = ids4c.parse("⿰贝⬚")
print(query.text)

database = ids4c.Database("yibai0")
for glyph in database.match(query):
    print(glyph)
```

解析 IDS 树并访问子节点：

```python
import ids4c

node = ids4c.parse("⿰亻言")
print(node.idc)
print([child.text for child in node.children])

for child in node.children:
    print(child.kind, child.text)
```

获取详细匹配结果：

```python
import ids4c

database = ids4c.Database("yibai0")
query = ids4c.parse("⿰贝⬚")
options = ids4c.QueryOptions()
options.track_match_paths = True

result = database.query(query, options)
print(result.equivalent_syntax)
for detail in result.matches:
    print(detail.glyph, detail.raw_ids, detail.match_paths)
```

注意：Python 扩展依赖构建它时选择的 CPython ABI、架构和工具链。发布二进制 Python 包时，需要针对目标 Python 版本和平台分别构建；本项目当前主要提供源码级构建入口。

## 测试

配置并构建后运行：

```powershell
ctest --test-dir build -C Release --output-on-failure
```

测试覆盖 IDS 数据库导入、私有库导入和重导入、导入错误诊断、IWDS 数据导入以及部分查询行为。上游完整数据不属于测试夹具，发布构建前仍应使用目标数据库进行实际查询验证。

## 贡献和变更记录

贡献流程见 [CONTRIBUTING.md](CONTRIBUTING.md)，版本变更见 [CHANGELOG.md](CHANGELOG.md)。

## 数据来源与参考

### IDS 数据库

本项目使用 YiBai 的 IDS 数据作为主要数据来源，精确匹配基础版本为 `ids_lv0.txt`：

- [yi-bai/ids](https://github.com/yi-bai/ids)

YiBai 项目说明中，`ids_lv0.txt` 区分所有笔画差异，`ids_lv1.txt` 合并部分笔画级差异，`ids_lv2.txt` 合并部分被认为必须统合的变体。请在分发由这些数据生成的数据库时，同时遵守上游项目的 MIT 许可证和其他说明。

### IWDS 数据

IWDS XML 数据来自：

- [yi-bai/iwds](https://github.com/yi-bai/iwds)

其中的 `iwds.xml` 用于构建 `unifiable.sqlite` 中的统合数据。IWDS 仓库还包含其数据格式文件、图像和生成文档；这些内容可能有独立的版权和使用条件，使用前请查阅上游仓库。

### IDS 语法参考

IDS 解析和查询语法参考：

- [NightFurySL2001/bai-ids](https://github.com/NightFurySL2001/bai-ids)

白式文档仓库以 MIT 许可证发布；语法实现和数据使用仍应以对应上游项目的最新说明为准。

### 第三方依赖

仓库中的 `third_party/` 包含项目直接使用的第三方源码。各依赖的许可证和版权声明以其目录中的文件或上游项目为准；仓库中的 [NOTICE](NOTICE) 汇总了已随仓库提供的声明和上游数据来源。发布二进制包时，应一并检查并保留适用的第三方许可证和 NOTICE 文件。

## 代码生成与贡献说明

部分代码曾由 OpenAI Codex 协助生成、重构和调试，最终由项目维护者审阅、修改并测试。Codex 不作为本项目的版权持有人或贡献者署名；源代码版权和贡献归属以仓库中的提交记录、许可证和项目维护者声明为准。

欢迎提交问题、测试用例和改进建议。涉及 IDS 解释、IWDS 统合或上游数据的问题，请尽量附上：

- 原始查询表达式；
- 数据库名称和导入来源；
- 期望结果与实际结果；
- CLI 的 `--explain` 输出或 JSON 详细结果；
- 可复现的最小 IDS 数据片段。
