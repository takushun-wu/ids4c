# ids4c

[中文说明](README.zh-CN.md)

ids4c is a C++ library and toolset for parsing and matching IDS (Ideographic Description Sequence) expressions for Chinese characters, with database import and IWDS-based fuzzy unification.

The project provides:

- ids4c: a GTKmm desktop application;
- ids4c-cli: a command-line query and database-import tool;
- ids4c.pyd: a Python extension module built with pybind11;
- C++ static libraries and public headers for integration into other applications.

The project source code is licensed under the Apache License, Version 2.0; see [LICENSE](LICENSE). Upstream IDS data, IWDS data, and syntax documentation have their own licenses and usage terms; see [Data Sources and References](#data-sources-and-references).

## Features

### IDS parsing and tree operations

- Parse ordinary IDS expressions, Bai-style IDS extensions, and query expressions;
- Preserve structure operators, components, strokes, suffixes, variables, and query parameters;
- Clone IDS trees, access child nodes, and compare tree structure;
- Calculate stroke counts from IDS expressions and use the database stroke cache.

### Query and matching

- Exact IDS matching and structural matching;
- Component-search expressions: `<search=...>`, `<any=...>`, and `<except=...>`;
- The component wildcard ⬚, variables such as `<var=...>`, and Unicode escapes `\uXXXX` / `\UXXXXXXXX`;
- Stroke conditions `<stroke=...>` and remaining-stroke conditions `<residue=...>`;
- Approximate stroke ranges with `~`, for example `<stroke=21~1>` and `<residue=12~2>`;
- Multiple HVExtract alternatives and same-IDS expression grouping;
- U+1F504 (🔄) replacement queries;
- Optional filtering of overlay structures represented by ⿻;
- Unicode-block, private-use, abstract-glyph, and custom-range filtering;
- Detailed match paths, equivalent-query indexes, and preprocessing-rule information.

### IWDS fuzzy unification

Database queries can use the following IWDS unification levels:

- `none`: no IWDS fuzzy unification;
- `srcseparation`: Source Code Separation;
- `lv1`: Source Code Separation plus component-level unification;
- `lv2`: additional IWDS component unification.

Unification is performed during query preprocessing. The resulting equivalent expressions are then passed to the IDS matching layer. Detailed results can retain the equivalent expression and match paths for use by higher-level applications.

## Query Syntax

The complete description of the specialized query syntax is in [docs/query-syntax.md](docs/query-syntax.md). The document covers `<search>`, `<any>`, `<except>`, stroke and residue conditions, variables, Unicode escapes, U+1F504 replacement queries, and the ▥/▤ structure extensions.

## Project Layout

```text
include/ids4c/       Public C++ headers
src/core/            IDS tree, parser, and core matching logic
src/db/              SQLite database, IDS import, and IWDS import
src/cli/             Command-line application
src/app/             GUI application entry point
src/ui/              GTKmm user interface
src/python/          pybind11 Python module
docs/                API and user documentation
tests/               CTest tests and test data
third_party/         Third-party source used directly by the project
```

## Requirements

Basic requirements:

- CMake 3.20 or newer;
- A C++11-capable compiler;
- SQLite 3;
- pkg-config;
- Boost. The CLI requires program_options; the GUI requires filesystem and system.

Additional target dependencies:

- GUI: GTKmm 3;
- Python binding: CPython development files and the pybind11 CMake package;
- CLI: Boost.Program_options.

On Windows, MSYS2 MinGW is recommended. Python, pybind11, SQLite, GTKmm, Boost, and CMake should target the same architecture and toolchain. The Python extension is a native CPython module and normally must be rebuilt for a different Python version, architecture, or toolchain.

## Build

### Build GUI, CLI, and Python binding

Python binding is enabled by default. If a compatible Python and pybind11 installation is available:

```powershell
cmake -S . -B build `
  -DIDS4C_BUILD_GUI=ON `
  -DIDS4C_BUILD_CLI=ON `
  -DIDS4C_BUILD_PYTHON=ON `
  -DCMAKE_BUILD_TYPE=Release

cmake --build build --config Release
```

If CMake cannot locate Python or pybind11 automatically, specify them explicitly:

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

### Build without Python binding

If the Python development environment is unavailable or the module is not needed:

```powershell
cmake -S . -B build `
  -DIDS4C_BUILD_GUI=ON `
  -DIDS4C_BUILD_CLI=ON `
  -DIDS4C_BUILD_PYTHON=OFF `
  -DCMAKE_BUILD_TYPE=Release

cmake --build build --config Release
```

Main CMake options:

| Option | Default | Description |
| --- | --- | --- |
| IDS4C_BUILD_GUI | ON | Build the GTKmm GUI |
| IDS4C_BUILD_CLI | ON | Build the CLI |
| IDS4C_BUILD_PYTHON | ON | Build the pybind11 module |
| IDS4C_ENABLE_LTO | OFF | Enable LTO/IPO for Release and RelWithDebInfo |

Runtime database files are intentionally not bundled with the public source repository. Prepare them by importing the upstream IDS and IWDS data as described below.

On Windows, the main build outputs are usually:

```text
ids4c.exe       GUI
ids4c-cli.exe   CLI
ids4c.pyd       Python extension module
```

The Python module is not a standalone application. Python must be able to find ids4c.pyd, and the runtime DLL directory must be on PATH:

```powershell
$env:PATH = "E:/msys64/mingw64/bin;$env:PATH"
$env:PYTHONPATH = "build"
python -c "import ids4c; print(ids4c.parse('⿰亻言').text)"
```

## Install

Install the built targets, headers, and documentation with:

```powershell
cmake --install build --prefix install
```

The installation includes the enabled applications, static libraries, headers, Python module, and documentation.

## Runtime Databases

ids4c uses SQLite databases. Database name NAME maps to:

```text
db/NAME.sqlite
```

For example, --database yibai0 opens db/yibai0.sqlite. If db/unifiable.sqlite exists, IWDS unification data is loaded from it as well.

Database files are generated by importing IDS source data. The recommended exact stroke-matching base is YiBai's ids_lv0.txt:

```powershell
New-Item -ItemType Directory -Force db
.\build\ids4c-cli.exe `
  --database yibai0 `
  --import path/to/ids_lv0.txt `
  --format yibai
```

Import IWDS XML:

```powershell
.\build\ids4c-cli.exe --import-iwds path/to/iwds.xml
```

Private IDS extensions can be added or reimported:

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

Import errors include the source line, IDS index, character position, and other diagnostics when available. Import formats are default and yibai.

## CLI

CLI arguments, database import, filters, output formats, and examples are documented in [docs/cli.md](docs/cli.md).

Minimal query:

```powershell
.\build\ids4c-cli.exe --database yibai0 --query "⿰贝⬚"
```

## Python API

Python API documentation is available in [English](docs/python-api.md) and [中文](docs/python-api-zh.md). Minimal example:

```python
import ids4c

query = ids4c.parse("⿰贝⬚")
database = ids4c.Database("yibai0")

for glyph in database.match(query):
    print(glyph)
```

The Python extension depends on the CPython ABI, architecture, and toolchain selected at build time. Binary distributions generally need separate builds for their target Python versions and platforms.

## Tests

After configuring and building:

```powershell
ctest --test-dir build -C Release --output-on-failure
```

Tests cover IDS database import, private-data import and reimport, import diagnostics, IWDS import, and selected query behavior. The complete upstream datasets are not test fixtures; validate with the target database before distribution.

## Contributing

See [CONTRIBUTING.md](CONTRIBUTING.md) for the build, test, documentation, and issue-reporting workflow. The project history is summarized in [CHANGELOG.md](CHANGELOG.md).

## Data Sources and References

### IDS database

The primary IDS data source is YiBai's IDS repository. The exact stroke-matching baseline is ids_lv0.txt:

- [yi-bai/ids](https://github.com/yi-bai/ids)

According to the upstream description, ids_lv0.txt distinguishes all stroke differences, ids_lv1.txt merges selected stroke-level differences, and ids_lv2.txt merges selected variants considered mandatory to unify. Distributions containing databases generated from this data must comply with the upstream MIT license and other notices.

### IWDS data

IWDS XML data comes from:

- [yi-bai/iwds](https://github.com/yi-bai/iwds)

iwds.xml is used to build the unification data in unifiable.sqlite. The IWDS repository also contains schema files, images, and document-generation sources, which may have separate copyright and usage terms.

### IDS syntax reference

The IDS syntax reference is:

- [NightFurySL2001/bai-ids](https://github.com/NightFurySL2001/bai-ids)

The bai-ids documentation repository is released under the MIT license. Always follow the current upstream terms for syntax and data usage.

### Third-party dependencies

third_party contains third-party source used directly by this project. Follow the license and copyright notices in each dependency and its upstream project. The repository's [NOTICE](NOTICE) file summarizes the bundled notices and upstream data sources. Binary distributions should retain applicable third-party licenses and NOTICE files.

## Code Generation and Contributions

Some code was generated, refactored, or debugged with assistance from OpenAI Codex. The project maintainer reviewed, modified, and tested the final code. Codex is not claimed as a copyright holder or project contributor; attribution follows the repository history, license, and maintainer statements.

Issues, test cases, and improvements are welcome. For IDS, IWDS, or upstream-data issues, include:

- the original query expression;
- database name and import source;
- expected and actual results;
- CLI --explain output or detailed JSON;
- a minimal reproducible IDS data sample.
