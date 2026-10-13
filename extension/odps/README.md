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

## Table kinds

The Storage API read path materializes only a physical, MaxCompute-managed
table. Most table shapes are readable; two are rejected at schema sniff (before
any data is read) because the reader cannot materialize them. The rejection is
an actionable error, never a silent empty/garbage scan:

| Table kind | Readable | Why / workaround |
|------------|----------|------------------|
| Standard table | Yes | |
| Partitioned table | Yes | Partition columns act as filters (`?partition=...`), not row columns. |
| Hash-clustered table | Yes | A full scan reads every bucket; clustering is a physical layout detail only. |
| Delta (transactional 2.0) table | No | Clears the kind gate but fails at read-session open: v1 splits by row offset, which ODPS rejects for transactional2.0 tables ("split by row count is not supported"). Needs a size-based split path (not yet implemented). |
| Materialized view | Yes | Physically stores its result, so it reads like a standard table (it is *not* a virtual view). |
| Logical view (`CREATE VIEW`) | No | Stores no physical data of its own (a saved SELECT the engine would have to run). Materialize it first (`CREATE TABLE ... AS SELECT`, or a materialized view) and read that table instead. |
| External table (`CREATE EXTERNAL TABLE`) | No | Its data lives in external object storage (e.g. OSS), not in MaxCompute-managed storage. Read the underlying files directly instead (e.g. `LOAD FROM "oss://..."` via the `httpfs` + `parquet`/`csv` extensions). |

The gate is driven by `IODPSTable::IsExternalTable()` / `IsVirtualView()`,
surfaced through the glue schema sniff (`OdpsGlueSchema.is_external_table` /
`is_virtual_view`) and turned into an error by
`OdpsSchemaConverter::unsupportedTableKindReason`. It fires at sniff, so it is
deterministic and never depends on the external location being populated.

Two clarifications on the shapes above. A **materialized view** is *not* a
virtual view (`IsVirtualView()` is false — it physically stores its result), so
it clears the gate and reads like a standard table; only a logical
`CREATE VIEW` is rejected. A **Delta (transactional 2.0) table** is a third
case: it clears this gate (it is a physical table) but the read fails when the
session opens, because v1 splits an unconfigured read by row offset and ODPS
rejects that for transactional2.0 tables ("split by row count is not supported
for transactional2.0 table"). The reader surfaces the real reason rather than a
silent partial scan; supporting Delta needs a size-based split path.

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
- **Timestamps are returned as UTC instants.** MaxCompute stores `DATETIME` /
  `TIMESTAMP` as instants and the Storage API transmits them as Arrow
  `timestamp` columns annotated with the project timezone (`odps.sql.timezone`,
  e.g. `Asia/Shanghai`); the raw payload is the epoch instant in UTC. The bridge
  decodes that instant into NeuG's timezone-naive millisecond `TIMESTAMP` and
  drops the Arrow timezone annotation, so values render as a **UTC wall-clock**.
  A value written as `2023-01-15 09:15:30` in a `UTC+8` project therefore reads
  back as `2023-01-15 01:15:30` — the *same instant*, sub-second fraction
  intact, only rendered in UTC rather than the project's local zone. `DATE` is a
  pure calendar date and is **not** shifted. `TIMESTAMP_NTZ` carries no timezone
  annotation, so its wall-clock value is preserved unchanged.
- A `TIMESTAMP` in seconds that would overflow NeuG's millisecond range is
  rejected with an explicit error rather than wrapping around.

### Date range

`DATE` maps to a 32-bit day count. A millisecond-based date value
(`date64`) that falls outside the 32-bit day range is rejected with an explicit
error.
