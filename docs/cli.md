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

`--version` prints the version, copyright, and license without opening a database:

```powershell
.\build\ids4c-cli.exe --version
```

## Query options

| Option | Short form | Description |
| --- | --- | --- |
| --database NAME | -d | SQLite database name without .sqlite |
| --query IDS | -q | Run an IDS query |
| --inspect GLYPH | | Show raw IDS, cached IDS, same-IDS glyphs, and containing glyphs |
| --explain | -e | Print detailed match information |
| --show-ids | | Alias for --explain |
| --no-match-paths | | Disable detailed match-path calculation |
| --profile | | Print query-stage timings and candidate counts to stderr |

GLYPH can be an actual character, U+XXXX, 0xXXXX, or an abstract glyph name.

## IWDS, region, and filters

| Option | Values/format | Description |
| --- | --- | --- |
| --unification-level | none, srcseparation, lv1, lv2 | IWDS fuzzy-unification level |
| --default-region | Region string | Fallback region or suffix when an exact component glyph is unavailable |
| --locale-suffix-order | `>` and `=` separated suffix list | Set locale-suffix fallback order; `.` means no suffix |
| --result-filter | all, ignore-lc-suffix, ignore-other-locales-base-only, ignore-other-locales-keep-ivs | Result suffix and locale filtering |
| --disable-same-ids-exclusion | No value | Allow same-IDS glyphs distinguished by `{glyph}` to match each other |
| --glyph-domain | all, unicode, private, abstract | Glyph-domain filter |
| --unicode-block | Comma-separated | Unicode-block filter |
| --ignore-overlay | No value | Ignore results whose matching path uses ⿻ |
| --overlap-match-mode | ignore, constrained | Ignore overlap modifiers (default), or constrain matrix states |
| --strict-enclosure-match | No value | Disable approximate enclosure layouts; retain exact reassociation |

--unicode-block accepts all, cjk, basic, ext-a through ext-j, compatibility, radicals, strokes, private-bmp, private-plane15, private-plane16, abstract, and other.

`ignore-other-locales-base-only` treats all IVS variants of a base code point as one result. `ignore-other-locales-keep-ivs` treats each base-code-point/variation-selector pair (including no selector) as a separate result. Strict locale filtering chooses among glyphs that actually matched the query; `--locale-suffix-order` does not make an unmatched base glyph eligible. The former `ignore-other-locales` value is no longer accepted.

With an explicit order such as `C=G>.>H>T`, `=` groups suffixes at the same priority and `>` orders fallback groups. If no unsuffixed definition exists, filtering can retain matching localized glyphs from the preferred available group. If an unsuffixed definition exists but does not match, a localized hit does not replace it.

`--ignore-overlay` excludes overlay matches; `--overlap-match-mode constrained` instead allows overlays and checks their modifiers. For wildcard details and enclosure rules, see [query-syntax.md](query-syntax.md). `--strict-enclosure-match` is independent of the IWDS level: it disables enclosure approximations, not IWDS fuzzy unification.

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

IDS imports print their current stage to stderr (reading, HV cache, stroke-neutral cache, component index, stroke counts, saving, and completion). Import failures report the source line, IDS index, character position, and other diagnostics when available.

### Caches after import

A successful IDS import builds the HV query cache, stroke-neutral composition cache, and first-level component index from the raw expressions. Raw IDS and derived data are stored in the same SQLite database; the caches and index should not be edited manually. Adding or reimporting private data updates the affected glyphs and index. Missing or older component indexes are rebuilt or migrated when the database is loaded.

If a database was generated from a new `ids_lv0.txt`, or if its derived data no longer matches the source file, reimport the database. The C++ API also provides `RebuildQueryCache()` to rebuild derived data from the raw IDS currently loaded in the database. The import report contains accepted expressions, cache entries, cache truncations, and detailed issues.

### Upgrading to 0.4.0

YiBai databases created with an older schema must be reimported from the source text using `--import ... --format yibai`. Schema 8 stores default/alternative definition-group metadata that cannot be recovered reliably from an older database. Rebuilding caches alone is insufficient. A full base import replaces the database contents; then reimport private extensions from their source files. Keep those sources before upgrading.

Variant lists such as `(.,T)` create separate unsuffixed and `T` records; identifiers are comma-separated. Alternative definitions are stored with `ids_entries.is_alternative`. This flag records the source group; it does not exclude those definitions from queries.

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
  --result-filter ignore-other-locales-keep-ivs `
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

Constrained overlap query:

```powershell
.\build\ids4c-cli.exe --database yibai0 `
  --overlap-match-mode constrained --query "⿻[?,x]丨日"
```

### Query profiling

```powershell
.\build\ids4c-cli.exe --database yibai0 `
  --unification-level lv2 --profile --query "<search=日,欠>"
```

The `[profile]` lines report milliseconds for preprocessing, same-IDS expansion, candidate indexing, scanning, result filtering, and detailed output construction. `index_used=A/B` means A of B index attempts used candidate filtering; it does not mean every query can use the index. Unsupported cases fall back to scanning. Timings are diagnostic measurements, not a performance guarantee; `--no-match-paths` can reduce detailed-output work. Profiling uses stderr and does not change the selected stdout result format.

## Runtime directory

Database name NAME maps to:

```text
db/NAME.sqlite
```

Before running the CLI, ensure that the target database exists in the current working directory. On Windows, if MinGW or GTKmm runtime DLLs are not on the system path, add the corresponding runtime directory to PATH.
