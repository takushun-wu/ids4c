# ids4c Specialized Query Syntax

[中文](query-syntax-zh.md)

This document describes the query, fuzzy-matching, and preprocessing syntax provided by ids4c on top of ordinary IDS. These forms are primarily query syntax and should not be used as a general-purpose persistence format for upstream IDS data.

For ordinary IDS operators and Bai-style IDS syntax, see:

- [NightFurySL2001/bai-ids](https://github.com/NightFurySL2001/bai-ids)

## Basic rules

Unicode escapes are processed first. The result is then parsed into IDS trees and query nodes. Query preprocessing can produce multiple equivalent expressions, which are passed to the matching layer independently.

Ordinary IDS structures use operators such as ⿰, ⿱, ⿲, ⿳, ⿸, and ⿺. The extensions below are query-layer constructs and do not necessarily represent one ordinary component in source IDS data.

## Component wildcard: ⬚

⬚ represents a replaceable IDS unit:

```text
⿰贝⬚
```

This searches for a structure whose left component is 贝 and whose right component is any matchable component. It does not mean any Unicode character; candidates are still constrained by IDS structure, database data, and filters.

## Component search

### `<search=...>`

`<search=...>` expresses an AND relationship. Comma-separated conditions must be found independently within the same candidate IDS expression:

```text
<search=长,马>
<search=幺,言,幺>
```

Matches from different IDS expressions are not combined into one result. A search expression can contain IDS structures and nested `<any=...>` expressions.

If a term itself contains a comma, quote it with single or double quotes:

```text
<search="<any=巾,忄>",亡>
<search='<any=巾,忄>',亡>
```

### `<any=...>`

`<any=...>` expresses an OR relationship; one matching term is enough:

```text
⿰<any=巾,忄>亡
<search=<any=巾,忄>,亡>
```

`<any=...>` does not allow `<residue=...>` inside it. Use `<search=...>` when multiple conditions must all be satisfied.

### `<except=...>`

`<except=...>` can appear at the outermost level or inside `<search=...>` and `<any=...>`. Directly listed terms exclude candidates matching any of them:

```text
<except=虫,貴>
```

To exclude candidates matching both 虫 and 貴, nest an AND search:

```text
<except=<search=虫,貴>>
```

At one level, a `<search>` or `<any>` expression may contain at most one `<except>` clause. Exclusion is still evaluated within the same candidate IDS expression.

## Stroke conditions

### `<stroke=...>`

`<stroke=...>` specifies the stroke-count range of the current IDS unit or glyph:

| Syntax | Meaning |
| --- | --- |
| `<stroke=12>` | Exactly 12 strokes |
| `<stroke=-12>` | At most 12 strokes |
| `<stroke=12->` | At least 12 strokes |
| `<stroke=12-18>` | From 12 through 18 strokes, inclusive |
| `<stroke=21~1>` | Centered at 21 with tolerance 1, that is 20 through 22 |

~N is converted to an inclusive range during preprocessing. Stroke counts are calculated from IDS expressions and cached in the database. If a count cannot be calculated reliably, it should not be treated as an absolute pre-filter.

### `<residue=...>`

`<residue=...>` is only valid in a `<search>` context and may occur at most once in one search expression. It means the total stroke count of the part remaining after the requested components are accounted for, without considering the remaining structure:

```text
<search=长,<residue=31->>
<search=水,<residue=12~2>>
```

`<residue=0>` means that no part remains. In applicable whole-glyph matching cases, it can also match the glyph itself and records with an equivalent IDS.

## Variables

`<var=...>` preserves component correspondence during one match. Variable names consist of ASCII letters, digits, and underscores:

```text
⿱<var=a>隹
⿱<var=_a>⿰<var=_b>⬚
```

A variable with the same name must correspond to the same IDS unit within one candidate match. Variables generated while importing IWDS use a separate namespace, commonly written as `<var=_a>`.

Full-width letters and digits are not automatically converted to variables during ordinary IDS import because they may be actual glyph components.

## HV origin-range annotations

The optional brackets on `▥` and `▤` record an ambiguity source produced by HVExtract; they are not ordinary components:

```text
▥[士=0:2](十一|口)
```

`士=0:2` means that the half-open child range `[0, 2)` in the derived structure originated from the glyph 士. Multiple origins are comma-separated, for example `[士=0:2,土=1:3]`. The `glyph` field is recorded only for source glyphs that require the explicit uniqueness distinction described in section 7.1 of IDS.pdf; callers should not add this annotation to an unambiguous structure.

This annotation mainly appears in the HV cache, detailed match results, or equivalent queries. The parser preserves it, and the matcher uses it to avoid treating 土 and an explicitly distinguished 士 as the same component. It is different from the combination-structure arguments in `▥(...)` / `▤(...)`; the ordinary `|` split marker in an arrangement must not be treated as a component.

## Raw IDS and derived caches

Database import preserves the raw IDS and builds derived query caches:

- raw IDS are the traceable definitions from sources such as `ids_lv0.txt`;
- the HVExtract cache stores multiple structural alternatives when one raw IDS expands in more than one way;
- the stroke-neutral composition cache ignores lowercase stroke suffix differences only during composition lookup, for example allowing `一t` to act as a composable component without changing the raw definition;
- private-data reimports and cache rebuilds regenerate derived caches from the raw IDS.

Consequently, `raw_ids` and `matched_ids` in a detailed result may differ. Use `match_source` and `preprocess_rules` to identify the cache source and preprocessing rules used.
## Unicode escapes

Before parsing, Unicode code points can be written using \uXXXX and \UXXXXXXXX:

```text
\u3402
\U0001F504赢贝女
```

The escape is converted to a Unicode character before IDS parsing and query preprocessing. The command shell, PowerShell, or Python may apply its own string-escaping rules as well.

## U+1F504 🔄 replacement query

🔄, written as \U0001F504 when the character cannot be entered directly, is a preprocessing form for replacing one component inside a glyph:

```text
\U0001F504赢贝女
```

This means: find 贝 in the IDS of 赢 and replace that component with 女. The form is query-only and does not require the corresponding raw expression to exist in the upstream IDS database.

## ▥, ▤, and HV expansion

▥(...) and ▤(...) are extended combination structures. The program also uses them to represent structure sets produced by HVExtract and query preprocessing:

```text
▥(牛⬚)
▤(丶一𠃊巾)
```

They are not structure-ignoring wildcards. Matching still checks component order, node ranges, and correspondence.

## Other extensions

The parser also recognizes:

- ㇯: a structure that subtracts a specified component;
- ⿻: overlay structure, filterable through query options or CLI --ignore-overlay;
- ⿾ and ⿿: extended structure nodes matched according to their parameters.

These forms belong primarily to the query layer. For interchange with other programs, store raw IDS expressions separately and keep specialized expressions as query input or preprocessing output.
