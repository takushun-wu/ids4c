# ids4c CLI

[中文](cli-zh.md)

This document describes the command-line options of ids4c-cli. For building, runtime database locations, and data sources, see the root README.

## Basic forms

```text
ids4c-cli --database NAME --query IDS [options]
ids4c-cli --database NAME --inspect GLYPH
ids4c-cli --database NAME --import SOURCE [--format default|yibai]
ids4c-cli --database NAME --import-private SOURCE [--format default|yibai]
ids4c-cli --database NAME --reimport-private SOURCE [--format default|yibai]
ids4c-cli --import-iwds SOURCE.xml
```

Show all options:

```powershell
.\build\ids4c-cli.exe --help
```

One main operation must be selected per invocation. All operations except --import-iwds require --database.

## Query options

| Option | Short form | Description |
| --- | --- | --- |
| --database NAME | -d | SQLite database name without .sqlite |
| --query IDS | -q | Run an IDS query |
| --inspect GLYPH | | Show raw IDS, cached IDS, same-IDS glyphs, and containing glyphs |
| --explain | -e | Print detailed match information |
| --show-ids | | Alias for --explain |
| --no-match-paths | | Disable detailed match-path calculation |

GLYPH can be an actual character, U+XXXX, 0xXXXX, or an abstract glyph name.

## IWDS, region, and filters

| Option | Values/format | Description |
| --- | --- | --- |
| --unification-level | none, srcseparation, lv1, lv2 | IWDS fuzzy-unification level |
| --default-region | Region string | Fallback region or suffix when an exact component glyph is unavailable |
| --locale-suffix-order | `>` and `=` separated suffix list | Set locale-suffix fallback order; `.` means no suffix |
| --result-filter | all, ignore-lc-suffix, ignore-other-locales | Result suffix and locale filtering |
| --glyph-domain | all, unicode, private, abstract | Glyph-domain filter |
| --unicode-block | Comma-separated | Unicode-block filter |
| --ignore-overlay | No value | Ignore results whose matching path uses ⿻ |

--unicode-block accepts all, cjk, basic, ext-a through ext-j, compatibility, radicals, strokes, private-bmp, private-plane15, private-plane16, abstract, and other.

## Output formats

| Option | Values | Description |
| --- | --- | --- |
| --output-format | text, csv, tsv, json | Query result format |
| --json-unicode | raw, escaped | Whether JSON emits non-ASCII characters as Unicode escapes |

CSV, TSV, and JSON include detailed fields. Text mode prints glyphs by default; combine it with --explain to print IDS, source, rules, and paths.

## Database import

### Import YiBai IDS

YiBai's ids_lv0.txt is recommended as the exact stroke-matching baseline:

```powershell
New-Item -ItemType Directory -Force db
.\build\ids4c-cli.exe `
  --database yibai0 `
  --import path/to/ids_lv0.txt `
  --format yibai
```

--format yibai also accepts ids_lv1.txt and ids_lv2.txt in the same format.

### Import the default format

```powershell
.\build\ids4c-cli.exe `
  --database custom `
  --import path/to/source.dat `
  --format default
```

### Import IWDS

```powershell
.\build\ids4c-cli.exe --import-iwds path/to/iwds.xml
```

This creates or updates db/unifiable.sqlite.

### Import private extensions

Append or update private IDS entries:

```powershell
.\build\ids4c-cli.exe `
  --database yibai0 `
  --import-private path/to/private.dat `
  --format default
```

Replace existing private IDS entries:

```powershell
.\build\ids4c-cli.exe `
  --database yibai0 `
  --reimport-private path/to/private.dat `
  --format default
```

Import failures report the source line, IDS index, character position, and other diagnostics when available.

### Caches after import

A successful IDS import builds the HV query cache and the stroke-neutral composition cache from the raw expressions. Raw IDS and derived caches are stored in the same SQLite database; the caches should not be edited manually. Adding or reimporting private data incrementally updates the affected glyphs.

If a database was generated from a new `ids_lv0.txt`, or if its derived data no longer matches the source file, reimport the database. The C++ API also provides `RebuildQueryCache()` to rebuild derived data from the raw IDS currently loaded in the database. The import report contains accepted expressions, cache entries, cache truncations, and detailed issues.
## Query examples

Basic structural query:

```powershell
.\build\ids4c-cli.exe `
  --database yibai0 `
  --query "⿰贝⬚"
```

Detailed IWDS lv2 query:

```powershell
.\build\ids4c-cli.exe `
  --database yibai0 `
  --unification-level lv2 `
  --explain `
  --query "⿰⬚𠧒"
```

Combined filters:

```powershell
.\build\ids4c-cli.exe `
  --database yibai0 `
  --result-filter ignore-other-locales `
  --glyph-domain unicode `
  --unicode-block "basic,ext-a" `
  --ignore-overlay `
  --query "⿰牛⬚"
```

JSON output:

```powershell
.\build\ids4c-cli.exe `
  --database yibai0 `
  --query "⿰贝⬚" `
  --output-format json `
  --json-unicode escaped
```

## Runtime directory

Database name NAME maps to:

```text
db/NAME.sqlite
```

Before running the CLI, ensure that the target database exists in the current working directory. On Windows, if MinGW or GTKmm runtime DLLs are not on the system path, add the corresponding runtime directory to PATH.
