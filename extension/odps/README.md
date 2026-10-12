# ODPS Extension

The ODPS extension reads Alibaba Cloud MaxCompute (ODPS) tables as an input
data source through the MaxCompute Storage API (Open Storage), so a table can
be scanned directly from Cypher:

```cypher
LOAD FROM "odps://project/table?partition=ds=20260101"
RETURN id, name;

COPY tbl FROM (LOAD FROM "odps://project/table" RETURN *);
```

The extension is read-only: it never writes back to or exports from MaxCompute.
This README documents the source-level type mapping and the precision/timezone
conventions that a user must understand before reading a table. The user-facing
guide lives at [`doc/source/extensions/odps.md`](../../doc/source/extensions/odps.md);
for build and SDK setup, see `CMakeLists.txt` and the feature spec under
`specs/008-odps-storage-extension/`.

## Type mapping

MaxCompute column types are mapped to NeuG types by `OdpsSchemaConverter`, and
the Arrow C Data Interface values are decoded by `odps_arrow_bridge`. A column
is readable only when both layers support its type.

| MaxCompute type | NeuG type | Notes |
|-----------------|-----------|-------|
| `TINYINT` / `SMALLINT` / `INT` | `INT32` | NeuG has no 8/16-bit primitive; narrow integers widen to `INT32`. |
| `BIGINT` | `INT64` | |
| `FLOAT` | `FLOAT` | |
| `DOUBLE` | `DOUBLE` | |
| `BOOLEAN` | `BOOL` | |
| `STRING` / `VARCHAR` / `CHAR` | `VARCHAR` | UTF-8 bytes are copied verbatim (no transcoding). |
| `DATE` | `DATE` | Days since the Unix epoch. |
| `DATETIME` / `TIMESTAMP` / `TIMESTAMP_NTZ` | `TIMESTAMP` (ms) | See [Precision and timezone](#precision-and-timezone). |
| `DECIMAL(p,s)` | `DOUBLE` | **LOSSY** — see [DECIMAL precision](#decimal-precision). |

### Unsupported types

The following MaxCompute types cannot be read. Each one fails fast with an
actionable error that names the column, the MaxCompute type, and a workaround,
rather than silently producing wrong values:

| MaxCompute type | Why it is rejected | Workaround |
|-----------------|--------------------|------------|
| `BINARY` | NeuG's read path has no binary/blob type, so raw bytes cannot be decoded losslessly. | Encode the column as `STRING` (e.g. BASE64) in MaxCompute. |
| `INTERVAL_YEAR_MONTH` / `INTERVAL_DAY_TIME` | INTERVAL values are not decoded from the ODPS Arrow stream yet. | Cast the column to a numeric or `STRING` in MaxCompute. |
| `ARRAY` / `MAP` / `STRUCT` | Complex/nested types are not supported; the reader decodes flat scalar columns only. | Flatten or extract the needed fields into scalar columns. |
| `JSON` | The Storage API read path does not support JSON columns. | Cast the column to `STRING` in MaxCompute. |

## Precision and timezone

### DECIMAL precision

> **Important — DECIMAL is read as DOUBLE and is lossy.**

MaxCompute `DECIMAL` defaults to `DECIMAL(38,18)` and can carry up to 38
significant digits. NeuG's read path has no exact decimal representation (the
type system exposes neither a decimal nor a blob type), so v1 maps `DECIMAL` to
`DOUBLE` to keep such columns readable. An IEEE-754 double carries only ~15–17
significant digits, therefore:

- Values whose magnitude or precision exceeds what a double can represent
  **silently lose precision** (e.g. a 20-digit integer, or a value with 18
  decimal places).
- This is a deliberate trade-off, not a bug. If exact decimal semantics are
  required, cast or round the column on the MaxCompute side (for example,
  project it to a scaled `BIGINT`, or to a `STRING`) before reading it.

The bridge decodes the Arrow `decimal128` layout (`d:precision,scale`) by
reading each 16-byte little-endian two's-complement integer and dividing by
`10^scale`. Only 128-bit decimals are accepted; a non-128-bit width is
rejected with a clear error.

### Timestamp precision and timezone

- `DATETIME`, `TIMESTAMP`, and `TIMESTAMP_NTZ` all collapse to NeuG
  `TIMESTAMP`, which has **millisecond** precision.
- Sub-millisecond precision is **truncated**, not rounded: nanosecond values
  are divided by 1,000,000 and microsecond values by 1,000, dropping the
  remainder.
- **No timezone shift is applied.** The wall-clock value emitted by MaxCompute
  is interpreted as-is (UTC-normalized milliseconds); the reader does not
  convert between local time and UTC. `TIMESTAMP_NTZ` (no time zone) is
  treated identically to `TIMESTAMP`.
- A `TIMESTAMP` in seconds that would overflow NeuG's millisecond range is
  rejected with an explicit error rather than wrapping around.

### Date range

`DATE` maps to a 32-bit day count. A millisecond-based date value
(`date64`) that falls outside the 32-bit day range is rejected with an explicit
error.
