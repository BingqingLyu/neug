# Implementation Plan: Iceberg Data Lake Extension

**Branch**: `004-iceberg-extension` | **Date**: 2026-05-12 | **Spec**: [spec.md](./spec.md)  
**Input**: Feature specification from `/specs/004-iceberg-extension/spec.md`

## Summary

Add read-only Apache Iceberg table support to NeuG via the extension framework. The extension resolves Iceberg metadata (snapshots, manifest lists, manifest files) to identify Parquet data files, then reads them using NeuG's existing Arrow-based reader infrastructure. Integrates with `LOAD FROM` syntax through auto-detection (probing for `metadata/` directory) or explicit `format='iceberg'` option.

## Technical Context

**Language/Version**: C++20 (consistent with NeuG codebase)  
**Primary Dependencies**: Apache Arrow (dataset, parquet, filesystem), RapidJSON (metadata parsing), NeuG Extension API  
**Storage**: Filesystem-based Iceberg tables (local, S3/OSS via VFS)  
**Testing**: pytest (Python integration tests via `tools/python_bind/`), ctest (C++ unit tests)

## Project Structure

```text
extension/iceberg/
├── CMakeLists.txt
├── include/
│   ├── iceberg_extension.h
│   ├── iceberg_read_function.h
│   ├── iceberg_options.h
│   ├── iceberg_sniffer.h
│   ├── metadata/
│   │   ├── iceberg_metadata.h          # Metadata JSON structures
│   │   ├── iceberg_manifest.h          # Manifest list & manifest file parsing
│   │   └── iceberg_snapshot.h          # Snapshot resolution logic
│   └── schema/
│       └── iceberg_type_mapper.h       # Iceberg → NeuG type mapping
├── src/
│   ├── iceberg_extension.cpp
│   ├── iceberg_read_function.cpp
│   ├── iceberg_options.cpp
│   ├── iceberg_sniffer.cpp
│   ├── metadata/
│   │   ├── iceberg_metadata.cpp
│   │   ├── iceberg_manifest.cpp
│   │   └── iceberg_snapshot.cpp
│   └── schema/
│       └── iceberg_type_mapper.cpp
└── tests/
    ├── CMakeLists.txt
    ├── test_iceberg_metadata.cpp
    ├── test_iceberg_manifest.cpp
    ├── test_iceberg_type_mapper.cpp
    └── resources/
        └── sample_iceberg_table/       # Pre-created test Iceberg table
            ├── metadata/
            │   ├── v1.metadata.json
            │   ├── snap-xxx-manifest-list.avro
            │   └── xxx-manifest.avro
            └── data/
                └── *.parquet
```

**Structure Decision**: Follows the existing `extension/parquet/` pattern — a self-contained directory under `extension/` with `include/`, `src/`, and `tests/` subdirectories. Metadata parsing and schema mapping are separated into sub-modules for clarity. The extension builds as a shared library (`neug_iceberg_extension.neug_extension`) via `build_extension_lib("iceberg")`.

## Data Model

This feature has the following data models:
1. Iceberg Table Metadata (metadata.json)
2. Manifest List Entry
3. Manifest File Entry (with Data File info)
4. Iceberg → NeuG Type Mapping
5. IcebergScanOptions (user-facing options)

### 1. Iceberg Table Metadata

**Data Structure**:

The root metadata file (`metadata/v<N>.metadata.json`) contains the table's schema, partition spec, and snapshot history:

```json
{
  "format-version": 2,
  "table-uuid": "abc-123",
  "location": "s3://bucket/warehouse/my_table",
  "schema": {
    "type": "struct",
    "fields": [
      {"id": 1, "name": "id", "required": true, "type": "long"},
      {"id": 2, "name": "name", "required": false, "type": "string"},
      {"id": 3, "name": "ts", "required": false, "type": "timestamptz"}
    ]
  },
  "current-snapshot-id": 3497810964824022504,
  "snapshots": [
    {
      "snapshot-id": 3497810964824022504,
      "timestamp-ms": 1714000000000,
      "manifest-list": "metadata/snap-349781-manifest-list.avro",
      "summary": {"operation": "append", "added-data-files": "2"}
    }
  ]
}
```

**Data Access & Update**:
- **Find latest metadata file**: Glob `metadata/v*.metadata.json`, sort numerically, pick highest version.
- **Get current snapshot**: Read `current-snapshot-id` field, find matching entry in `snapshots` array.
- **Get specific snapshot by ID**: Linear scan `snapshots` array matching `snapshot-id`.
- **Get specific snapshot by timestamp**: Linear scan `snapshots` array, find latest with `timestamp-ms <= target`.
- **Extract schema**: Parse `schema.fields` array into typed column descriptors.

### 2. Manifest List Entry

**Data Structure**:

The manifest list (Avro file pointed to by snapshot's `manifest-list`) contains entries for each manifest file:

```json
{
  "manifest_path": "metadata/abc-manifest.avro",
  "manifest_length": 8192,
  "partition_spec_id": 0,
  "content": 0,
  "added_snapshot_id": 3497810964824022504,
  "added_data_files_count": 2,
  "existing_data_files_count": 0,
  "deleted_data_files_count": 0,
  "partitions": [
    {"contains_null": false, "lower_bound": "2025-01-01", "upper_bound": "2025-01-31"}
  ]
}
```

Where `content` field: 0 = data manifest, 1 = delete manifest.

**Data Access & Update**:
- **List all manifests for snapshot**: Read Avro manifest-list, iterate entries.
- **Filter by content type**: Select entries with `content == 0` for data files, `content == 1` for delete files.
- **Partition pruning**: Compare query predicates against `partitions[].lower_bound/upper_bound` to skip irrelevant manifests.

### 3. Manifest File Entry (Data File)

**Data Structure**:

Each manifest file (Avro) contains entries describing individual data files:

```json
{
  "status": 1,
  "data_file": {
    "file_path": "data/00001-1-abc.parquet",
    "file_format": "PARQUET",
    "record_count": 50000,
    "file_size_in_bytes": 2048000,
    "partition": {"date": "2025-01-15"},
    "column_sizes": {"1": 100000, "2": 200000},
    "value_counts": {"1": 50000, "2": 49500},
    "null_value_counts": {"1": 0, "2": 500},
    "lower_bounds": {"1": "\u0001\u0000\u0000\u0000"},
    "upper_bounds": {"1": "\u00c8\u0000\u0000\u0000"}
  }
}
```

Where `status`: 0 = existing, 1 = added, 2 = deleted.

**Data Access & Update**:
- **Collect active data files**: Read manifest, include entries with `status` 0 or 1, exclude `status` 2.
- **Column-level pruning**: Use `lower_bounds`/`upper_bounds` per column to skip files not matching filter predicates.
- **Resolve full path**: Concatenate table `location` + `data_file.file_path` (relative paths) or use `file_path` directly (absolute paths).

### 4. Iceberg → NeuG Type Mapping

**Data Structure**:

A compile-time lookup table mapping Iceberg type strings to NeuG LogicalTypeID:

| Iceberg Type | NeuG LogicalTypeID | Notes |
|---|---|---|
| `boolean` | `BOOL` | Direct |
| `int` | `INT32` | Direct |
| `long` | `INT64` | Direct |
| `float` | `FLOAT` | Direct |
| `double` | `DOUBLE` | Direct |
| `decimal(p,s)` | `DECIMAL` | Preserves precision/scale |
| `date` | `DATE` | Direct |
| `time` | `STRING` | ISO format "HH:MM:SS.sss" |
| `timestamp` | `TIMESTAMP` | Without timezone |
| `timestamptz` | `TIMESTAMP_TZ` | With timezone |
| `string` | `STRING` | Direct |
| `uuid` | `UUID` | Direct |
| `fixed(L)` | `BLOB` | Direct |
| `binary` | `BLOB` | Direct |
| `struct<...>` | `STRING` | JSON stringified |
| `list<...>` | `STRING` | JSON stringified |
| `map<...>` | `STRING` | JSON stringified |

**Data Access & Update**:
- **Map single type**: Parse Iceberg type string → match against known patterns → return LogicalTypeID.
- **Map full schema**: Iterate Iceberg schema fields, map each type, produce vector of (name, LogicalTypeID) pairs.
- **Handle nested types**: For struct/list/map, return STRING type and mark column for JSON serialization during read.

### 5. IcebergScanOptions

**Data Structure**:

User-facing options passed via `LOAD FROM` inline parameters:

```cpp
struct IcebergScanOptions {
    std::optional<int64_t> snapshot_id;         // SNAPSHOT_ID=12345
    std::optional<std::string> snapshot_ts;     // SNAPSHOT_TIMESTAMP='2025-01-01T00:00:00'
    bool format_explicit = false;               // true if user wrote (format='iceberg')
};
```

**Data Access & Update**:
- **Parse from LOAD FROM options**: Extract known keys (SNAPSHOT_ID, SNAPSHOT_TIMESTAMP, format) from options map.
- **Validate**: If both snapshot_id and snapshot_ts are set, raise error (mutually exclusive).
- **Default behavior**: If neither is set, use `current-snapshot-id` from metadata.

## Algorithm Model

This feature has the following algorithm models:
1. Iceberg Format Auto-Detection
2. Metadata Resolution & Snapshot Selection
3. Data File Collection (with Delete File Handling)
4. Manifest-Level Predicate Pruning

### 1. Iceberg Format Auto-Detection

**Algorithm Target**: Determine whether a given path is an Iceberg table without explicit user annotation, enabling transparent `LOAD FROM "path" RETURN *` usage.

**Algorithm Details**:

When `LOAD FROM` receives a path without explicit format, the system runs format sniffers in priority order. The Iceberg sniffer probes:

```
Input: path (string), filesystem (VFS)
Output: bool (is_iceberg_table)

1. Construct candidate = path + "/metadata/"
2. Try to list files in candidate directory
3. If listing fails (directory doesn't exist) → return false
4. Search for files matching pattern "v*.metadata.json" in listing
5. If at least one match found → return true
6. Otherwise → return false
```

**Example**:
- Path: `/data/warehouse/my_table`
- Probe: `/data/warehouse/my_table/metadata/` → exists, contains `v1.metadata.json` → detected as Iceberg

**Priority**: Iceberg sniffer runs AFTER Parquet sniffer (which checks `.parquet` extension) but BEFORE CSV sniffer. A path ending in `.parquet` will never trigger Iceberg detection.

### 2. Metadata Resolution & Snapshot Selection

**Algorithm Target**: Given an Iceberg table path and optional snapshot options, resolve the full metadata chain from `metadata.json` → manifest list → manifest files, producing a list of data file paths to read.

**Algorithm Details**:

```
Input: table_path, scan_options (IcebergScanOptions), filesystem (VFS)
Output: (schema, list<DataFileEntry>, list<DeleteFileEntry>)

Phase A: Find metadata file
1. List files in table_path/metadata/ matching "v*.metadata.json"
2. Sort by version number (extract N from "vN.metadata.json")
3. Select highest version → current_metadata

Phase B: Select snapshot
4. Parse current_metadata JSON
5. If scan_options.snapshot_id is set:
     Find snapshot in metadata.snapshots where snapshot-id == target
     If not found → ERROR "Snapshot ID not found. Available: [list IDs]"
6. Else if scan_options.snapshot_ts is set:
     Find latest snapshot where timestamp-ms <= parse(target_ts)
     If none → ERROR "No snapshot at or before given timestamp"
7. Else:
     Use metadata.current-snapshot-id

Phase C: Read manifest list
8. Resolve manifest_list_path = table_path + "/" + snapshot.manifest-list
9. Read Avro manifest list file → list<ManifestListEntry>

Phase D: Read manifest files
10. For each manifest_entry in manifest_list:
      If manifest_entry.content == 0 → data manifest
      If manifest_entry.content == 1 → delete manifest
11. For each manifest, read Avro file → collect DataFileEntry / DeleteFileEntry
12. Filter: include only status == 0 (existing) or status == 1 (added)

Phase E: Extract schema
13. Parse metadata.schema.fields → apply type mapping (Data Model 4)

Return (schema, data_files, delete_files)
```

**Example**:
- Table: `/data/my_table`, no snapshot options
- Finds `v3.metadata.json` → `current-snapshot-id: 999`
- Snapshot 999 points to `snap-999-manifest-list.avro`
- Manifest list has 2 data manifests → produces 5 data file paths
- Returns schema + 5 data file paths + 0 delete file paths

### 3. Data File Collection (with Delete File Handling)

**Algorithm Target**: Read collected Parquet data files and apply Iceberg delete files (positional and equality deletes) to exclude deleted rows from the final result.

**Algorithm Details**:

```
Input: data_files (list<DataFileEntry>), delete_files (list<DeleteFileEntry>), schema, filesystem
Output: Arrow RecordBatchReader (merged, filtered stream)

Phase A: Group deletes by data file
1. For each delete_file:
     If delete_file.content == "position_deletes":
       Read delete Parquet file → columns: [file_path, pos]
       Group by file_path → map<file_path, sorted_set<row_position>>
     If delete_file.content == "equality_deletes":
       Read delete Parquet file → equality predicates on specified columns
       Build filter expression per affected data file

Phase B: Read data files with delete application
2. For each data_file in data_files:
     Resolve full path via VFS
     Create ArrowReader with ArrowParquetOptionsBuilder
     If positional deletes exist for this file:
       Read with row numbering, skip positions in delete set
     If equality deletes exist:
       Apply equality filter to exclude matching rows
     Yield filtered record batches

Phase C: Nested type JSON serialization
3. For columns marked as nested (struct/list/map):
     Post-process each batch: serialize Arrow struct/list/map columns to JSON strings
```

**Example** (positional delete):
- Data file `00001.parquet` has 1000 rows
- Delete file says: positions [5, 42, 999] are deleted for `00001.parquet`
- Read all rows from `00001.parquet`, skip rows at positions 5, 42, 999
- Return 997 rows

### 4. Manifest-Level Predicate Pruning

**Algorithm Target**: Skip reading entire manifest files and data files whose partition bounds or column statistics prove they cannot contain matching rows, reducing I/O.

**Algorithm Details**:

```
Input: manifest_list_entries, query_predicates (from WHERE clause), partition_spec
Output: pruned list of manifest entries to scan

Level 1: Manifest-list pruning
1. For each manifest_entry in manifest_list:
     If manifest_entry.partitions is available:
       For each predicate on partition columns:
         Compare predicate value against partition summary (lower_bound, upper_bound)
         If predicate cannot match range → skip this manifest entirely

Level 2: Data-file pruning (within each manifest)
2. For each data_file_entry in manifest:
     If data_file_entry.lower_bounds / upper_bounds available for predicate columns:
       Evaluate predicate against column bounds
       If predicate cannot match → skip this data file

Level 3: Parquet row-group pruning (delegated to Arrow)
3. For remaining data files, pass predicates to Arrow Parquet reader
   Arrow handles row-group level statistics filtering internally
```

**Example**:
- Query: `WHERE date = '2025-03-15'`
- Manifest A: partitions summary `lower=2025-01, upper=2025-02` → SKIP (no March data)
- Manifest B: partitions summary `lower=2025-03, upper=2025-04` → READ
- Within Manifest B, data file `part-001.parquet`: bounds `[2025-03-10, 2025-03-20]` → READ
- Within Manifest B, data file `part-002.parquet`: bounds `[2025-04-01, 2025-04-15]` → SKIP
