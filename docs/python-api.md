# ids4c Python API

[中文](python-api-zh.md)

This document describes the Python binding. For building, Python-version/toolchain compatibility, runtime databases, and data sources, see the root [README.md](../README.md). For query syntax, see [query-syntax.md](query-syntax.md).

## Import the module

```python
import ids4c
```

Main interfaces:

- parse(): parse an IDS or query expression;
- equal(): compare two IDS trees exactly;
- IDSNode: an owning IDS node object;
- Database: perform database operations and IDS queries;
- QueryOptions, MatchDetail, MatchPath, and QueryResult: query options and structured results.

## Parse IDS

### parse(text)

```python
node = ids4c.parse("⿰亻言")

print(node.kind)                                # pattern
print(node.text)                                # ⿰亻言
print(node.idc)                                 # ⿰
print([child.text for child in node.children]) # ['亻', '言']
```

The parser also accepts supported query expressions:

```python
ids4c.parse("⿰贝⬚")
ids4c.parse("<search=长,<residue=31->>")
ids4c.parse("⿰<any=巾,忄>亡")
ids4c.parse("⿱<var=a>隹")
```

Invalid input raises ValueError. The message may include the Unicode code-point position:

```python
try:
    ids4c.parse("⿰一")
except ValueError as error:
    print(error)
```

### equal(left, right)

This performs exact IDS-tree equality. It does not apply IWDS, HVExtract, or other fuzzy equivalence:

```python
assert ids4c.equal(
    ids4c.parse("⿰亻言"),
    ids4c.parse("⿰亻言"),
)
```

## IDSNode

Common properties:

| Property | Description |
| --- | --- |
| kind | Node type such as stroke, ideograph, pattern, search, or variable |
| text | Serialized IDS/query expression |
| children | Pattern children or search terms |
| idc | Structure operator such as ⿰ or ⿱ |
| idc_type | Numeric C++ IDCtype value |
| terms | Search-expression terms |
| except_terms | Exclusion terms |
| mode | all, any, or except |

Ideograph nodes also expose glyph, codepoint, variation_selector, suffix, and abstract_name.

Stroke nodes expose stroke_data, break_positions, cross_data, and enclosed. Search-parameter nodes expose param, stroke_minimum, and stroke_maximum. Variable nodes expose variable_name.

```python
glyph = ids4c.parse("嬴")
print(glyph.glyph, hex(glyph.codepoint), glyph.suffix)

pattern = ids4c.parse("⿰亻言")
print([child.text for child in pattern.children])
```

### Clone and replace children

```python
query = ids4c.parse("⿰亻言")
copy = query.clone()
changed = query.with_children(ids4c.parse("⿰亻信").children)

assert ids4c.equal(copy, query)
print(changed.text)  # ⿰亻信
```

Returned nodes own independent data and do not depend on the lifetime of the original node.

## Database

### Open a database and read IDS

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

raw_ids() returns original IDS expressions. ids() returns imported cached/preprocessed IDS expressions. Both return list[IDSNode].

### HV expansion and equivalent queries

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

hv_extract() may return multiple results. Use maximum to limit expansion.

### Basic matching

```python
query = ids4c.parse("⿰贝⬚")
matches = database.match(query)

print(matches[:5])
```

match() returns only the matching glyph strings.

## Query options

```python
options = ids4c.QueryOptions()
options.track_match_paths = True
options.filter.ignore_overlay_structure = True
options.filter.result_filter = ids4c.ResultFilter.IGNORE_OTHER_LOCALES_KEEP_IVS
options.filter.glyph_domain = ids4c.GlyphDomain.UNICODE
options.filter.unicode_blocks = [ids4c.UnicodeBlock.CJK_BASIC]

matches = database.match(query, options)
```

QueryOptions:

- filter: a FilterOptions object;
- track_match_paths: whether to calculate detailed match paths. Set it to False when only glyph names are needed.

FilterOptions:

- ignore_overlay_structure: ignore results whose matching path uses ⿻;
- result_filter: ALL, IGNORE_LC_SUFFIX, IGNORE_OTHER_LOCALES_BASE_ONLY, or IGNORE_OTHER_LOCALES_KEEP_IVS;
- glyph_domain: ALL, UNICODE, PRIVATE, or ABSTRACT;
- unicode_blocks: one or more UnicodeBlock values;
- custom_ranges: names of registered custom ranges.

The current Python binding can select ranges registered by the host application, but does not yet expose range registration.
`IGNORE_OTHER_LOCALES_BASE_ONLY` collapses IVS variants of a base code point. `IGNORE_OTHER_LOCALES_KEEP_IVS` keeps each base/variation-selector pair, including the no-selector form, independent. Strict locale filtering selects only among results that matched the query. `options.filter.locale_suffix_fallback_order` can specify the suffix priority; the old `IGNORE_OTHER_LOCALES` enum value is unavailable.
Available enums include ResultFilter, GlyphDomain, UnicodeBlock, IWDSUnificationLevel, and DatabaseFormat.

## Detailed results

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

MatchDetail properties:

| Property | Description |
| --- | --- |
| glyph | Matched glyph |
| matched_ids | IDS expression that matched successfully |
| raw_ids | Original IDS from the database |
| match_kind | Category such as exact, structural, or component search |
| match_source | Source such as raw IDS or HV cache |
| preprocess_rules | Names of the preprocessing rules used |
| match_paths | A list of MatchPath objects |

MatchPath properties:

- kind: path type;
- index: index when applicable, otherwise None;
- query_expression_index: index of the equivalent query that matched, otherwise None;
- query_path: path in the query expression;
- path: path in the candidate IDS.

Paths use the compact format, for example /child[0:1]. Set track_match_paths to False when paths are not needed.

### query(query, options=None)

Combine equivalent queries and detailed matches:

```python
result = database.query(query, options)

print(result.equivalent_syntax)
for detail in result.matches:
    print(detail.glyph, detail.match_kind)
```

QueryResult provides:

- equivalent_syntax: the preprocessed equivalent queries;
- matches: a list of MatchDetail objects;
- to_dict(): a dictionary suitable for application-level JSON serialization.

MatchDetail and MatchPath also provide to_dict().

## Configuration

```python
config = database.config
config.fuzzy_match.unification_level = (
    ids4c.IWDSUnificationLevel.SOURCE_CODE_SEPARATION
)
config.fuzzy_match.default_region = "G"
config.fuzzy_match.stroke_neutral_composition = True
config.fuzzy_match.exclude_non_equivalent_same_ids = True
config.misc.enable_cache = True
config.misc.sym_fallback = True
config.misc.suffix_is_alt_form = False

database.config = config
```

DatabaseConfig contains fuzzy_match and misc configuration groups. Changes affect subsequent operations on that Database object.
`exclude_non_equivalent_same_ids` defaults to True: glyphs explicitly distinguished by `{glyph}` are not unified merely because they share an IDS expression. Set it to False to allow those same-IDS matches.

## Import data

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

- import_file(): import IDS data;
- import_private_file(): append or replace private extension data;
- replace=True: replace existing private extension data.

Import failures raise RuntimeError. Read database.last_error and database.last_import_report for the latest error and structured diagnostics.

`last_import_report` is a dictionary containing fields such as `input_lines`, `data_lines`, `source_expressions`, `accepted_expressions`, `rejected_expressions`, `query_cache_entries`, `rebuilt_cache_glyphs`, `cache_truncations`, and `issues`. Each item in `issues` includes `line`, `ids_index`, `character_index`, `glyph`, `expression`, and `message`, which can be used to display the exact import location.

After import, the database builds the HV cache, stroke-neutral composition cache, and first-level component index from the raw IDS. `raw_ids()` returns original definitions, while `ids()` returns query-cache entries. A missing or older index is rebuilt or migrated when loading the database. To regenerate derived data from the raw definitions, reimport the database or use the C++ API `RebuildQueryCache()`. The C++ import API supports stage callbacks; they are not exposed by this Python binding.
## Ownership and threading

- IDSNode objects and database-returned objects own their data;
- do not query and import through the same Database object concurrently without external synchronization;
- for multithreaded use, create one Database object per thread or provide a lock in the caller;
- the binding is intended as a foundation for IDS-tree processing, including future IDS-to-stroke-sequence expansion.

The binding currently targets basic functionality and stable data structures; it does not promise to expose every internal C++ interface.
