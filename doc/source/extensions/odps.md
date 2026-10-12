# ODPS / MaxCompute

The ODPS extension reads Alibaba Cloud MaxCompute (ODPS) tables directly as an
input data source through the MaxCompute Storage API (Open Storage). Once
loaded, a MaxCompute table can be scanned from Cypher with the same `LOAD FROM`
syntax used for CSV/Parquet/JSON, and imported into the graph with
`COPY ... FROM (LOAD FROM ...)`.

The extension is **read-only**: it never writes back to or exports from
MaxCompute.

> **Build requirement:** the data plane depends on the Alibaba Cloud
> `aliyun-odps-sdk-cpp`, so this extension only reads real tables when NeuG is
> built with the ODPS SDK enabled (`-DNEUG_WITH_ODPS_SDK=ON`). A build without
> the SDK still routes `odps://` addresses to the extension, but any live read
> fails with a clear *"requires the ODPS SDK build"* error. The Storage API
> service endpoint is normally only reachable from inside a VPC (intranet).

## Install Extension

```cypher
INSTALL ODPS;
```

## Load Extension

```cypher
LOAD ODPS;
```

## Credentials and Endpoint

Credentials are resolved from the environment by the C++ connection layer — they
are **never** part of the query text, so an AccessKey secret cannot leak into a
query log or an error message.

| Environment variable | Alias | Purpose |
| -------------------- | ----- | ------- |
| `ODPS_ACCESS_KEY_ID` | `ALIBABA_CLOUD_ACCESS_KEY_ID` | AccessKey ID |
| `ODPS_ACCESS_KEY_SECRET` | `ALIBABA_CLOUD_ACCESS_KEY_SECRET` | AccessKey secret |
| `ODPS_ENDPOINT` | — | Storage API service endpoint (usually VPC/intranet) |
| `ODPS_TUNNEL_ENDPOINT` | — | Optional tunnel endpoint |
| `ODPS_PROJECT` | — | Default project when the address omits it |
| `ODPS_QUOTA_NAME` | — | Storage API quota (empty = pay-as-you-go default) |
| `ODPS_REGION_ID` | — | Optional region id |

## Table Address Grammar

```text
odps://[project.][schema.]table[?partitionSpec]
```

- At most three dot-separated segments: `project`, `schema`, `table`.
- A missing `schema` defaults to `default`; a missing `project` is taken from
  the connection default (`ODPS_PROJECT` or the `project` option).
- Everything after `?` is a partition spec:
  - `,` or `&` separates **multiple** partitions (all are read);
  - `/` separates the **levels** within one partition (e.g. `pt=1/ds=x`),
    matching the SDK's partition-spec dialect;
  - each level is a non-empty `key=value` pair.
- Read knobs may also be given as options: `project`, `schema`, `table`,
  `partitions`, `split_size_mb`, `quota_name`.

## Loading from an ODPS Table

Read all columns:

```cypher
LOAD FROM "odps://my_project.my_table"
RETURN *;
```

Project and reorder columns (column pruning is pushed down to MaxCompute):

```cypher
LOAD FROM "odps://my_project.my_table"
RETURN id, name, price;
```

Read a single partition (partition pruning is pushed down):

```cypher
LOAD FROM "odps://my_project.my_table?pt=20260101"
RETURN id, name, price;
```

Filter rows — a pushable predicate (`price > 5.0`) is translated to an ODPS
filter and applied server-side; anything the converter cannot push down falls
back to engine-side filtering with identical results:

```cypher
LOAD FROM "odps://my_project.my_table"
WHERE price > 5.0
RETURN id, name, price;
```

> **Note:** all relational operations supported by `LOAD FROM` — projection,
> `WHERE`, aggregation, sorting, and limiting — work the same way with ODPS
> sources. See the [LOAD FROM reference](../data_io/load_data).

## Importing into the Graph

Import into a node table:

```cypher
CREATE NODE TABLE item(id INT64 PRIMARY KEY, name STRING, price DOUBLE);
COPY item FROM (LOAD FROM "odps://my_project.my_table" RETURN id, name, price);
```

Import into an edge table — the first two RETURN columns are bound to the
source/destination primary keys, and any remaining columns become edge
properties:

```cypher
COPY knows FROM (
  LOAD FROM "odps://my_project.knows" RETURN src, dst, since
) (from="person", to="person");
```

## Type Mapping

MaxCompute column types are mapped to NeuG types as follows. A column is
readable only when both the schema mapping and the Arrow value decoding support
its type.

| MaxCompute type | NeuG type | Notes |
| --------------- | --------- | ----- |
| `TINYINT` / `SMALLINT` / `INT` | `INT32` | NeuG has no 8/16-bit primitive; narrow integers widen to `INT32`. |
| `BIGINT` | `INT64` | |
| `FLOAT` | `FLOAT` | |
| `DOUBLE` | `DOUBLE` | |
| `BOOLEAN` | `BOOL` | |
| `STRING` / `VARCHAR` / `CHAR` | `STRING` | UTF-8 bytes are copied verbatim (no transcoding). `CHAR` keeps MaxCompute's fixed-length space padding. |
| `DATE` | `DATE` | Days since the Unix epoch. |
| `DATETIME` / `TIMESTAMP` / `TIMESTAMP_NTZ` | `TIMESTAMP` (ms) | See [Precision and timezone](#precision-and-timezone). |
| `DECIMAL(p,s)` | `DOUBLE` | **LOSSY** — see [DECIMAL precision](#decimal-precision). |

### DECIMAL precision

> **Important — DECIMAL is read as DOUBLE and is lossy.**

MaxCompute `DECIMAL` defaults to `DECIMAL(38,18)` and can carry up to 38
significant digits. NeuG's read path has no exact decimal representation, so the
ODPS reader maps `DECIMAL` to `DOUBLE` to keep such columns readable. An
IEEE-754 double carries only ~15–17 significant digits, therefore values whose
magnitude or precision exceeds that **silently lose precision** (for example a
20-digit integer, or a value with 18 decimal places).

This is a deliberate trade-off, not a bug. If exact decimal semantics are
required, cast or round the column on the MaxCompute side before reading it —
for example, project it to a scaled `BIGINT` or to a `STRING`.

## Precision and timezone

- `DATETIME`, `TIMESTAMP`, and `TIMESTAMP_NTZ` all collapse to NeuG `TIMESTAMP`,
  which has **millisecond** precision.
- Sub-millisecond precision is **truncated**, not rounded: nanosecond values are
  divided by 1,000,000 and microsecond values by 1,000, dropping the remainder.
- **No timezone shift is applied.** The wall-clock value emitted by MaxCompute is
  interpreted as-is (UTC-normalized milliseconds); the reader does not convert
  between local time and UTC. `TIMESTAMP_NTZ` (no time zone) is treated
  identically to `TIMESTAMP`.
- A `TIMESTAMP` in seconds that would overflow NeuG's millisecond range, or a
  date outside the 32-bit day range, is rejected with an explicit error rather
  than wrapping around.

## Unsupported Types

The following MaxCompute types cannot be read. Each one fails fast with an
actionable error naming the column, the MaxCompute type, and a workaround,
rather than silently producing wrong values:

| MaxCompute type | Why it is rejected | Workaround |
| --------------- | ------------------ | ---------- |
| `BINARY` | NeuG's read path has no binary/blob type, so raw bytes cannot be decoded losslessly. | Encode the column as `STRING` (e.g. BASE64) in MaxCompute. |
| `INTERVAL_YEAR_MONTH` / `INTERVAL_DAY_TIME` | INTERVAL values are not decoded from the ODPS Arrow stream yet. | Cast the column to a numeric or `STRING` in MaxCompute. |
| `ARRAY` / `MAP` / `STRUCT` | Complex/nested types are not supported; the reader decodes flat scalar columns only. | Flatten or extract the needed fields into scalar columns. |
| `JSON` | The Storage API read path does not support JSON columns. | Cast the column to `STRING` in MaxCompute. |

## Limitations

- **Read-only.** Import from MaxCompute is supported; export/write back is not.
- **SDK build required.** Live reads need a NeuG build with the ODPS SDK
  enabled; the Storage API endpoint is normally VPC/intranet only.
- **Flat scalar columns only.** Nested and unsupported types must be reshaped on
  the MaxCompute side (see [Unsupported Types](#unsupported-types)).

For the source-level type mapping and build details, see
`extension/odps/README.md` in the repository.
