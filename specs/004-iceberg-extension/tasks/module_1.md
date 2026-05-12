# Module 1: Iceberg Table Reading (P1)

**Goal**: Enable users to query Iceberg tables directly through NeuG's `LOAD FROM` Cypher syntax, including metadata resolution, schema inference, snapshot selection, column projection, predicate pushdown, and delete file handling.

**Assignee**: 
**Label**: iceberg
**Milestone**: 
**Project**: 

## [F004-T101] Iceberg Type Mapper

**description**: Implement the compile-time type mapping from Iceberg schema type strings to NeuG `LogicalTypeID`. This is a foundational utility used by metadata parsing and schema inference.

**details**:
* Create `extension/iceberg/include/schema/iceberg_type_mapper.h` and `extension/iceberg/src/schema/iceberg_type_mapper.cpp`
* Implement function `LogicalTypeID mapIcebergType(const std::string& icebergType)` that maps:
  - `boolean` → `BOOL`, `int` → `INT32`, `long` → `INT64`, `float` → `FLOAT`, `double` → `DOUBLE`
  - `decimal(p,s)` → `DECIMAL` (extract precision/scale via regex)
  - `date` → `DATE`, `timestamp` → `TIMESTAMP`, `timestamptz` → `TIMESTAMP_TZ`
  - `time` → `STRING` (NeuG has no native TIME type)
  - `string` → `STRING`, `uuid` → `UUID`, `binary` → `BLOB`, `fixed(L)` → `BLOB`
  - `struct<...>`, `list<...>`, `map<...>` → `STRING` (mark for JSON serialization)
* Implement function `std::vector<std::pair<std::string, LogicalTypeID>> mapIcebergSchema(const rapidjson::Value& schemaFields)` that maps a full Iceberg schema fields array
* Handle unknown types by throwing a descriptive error with the unrecognized type string
* Use RapidJSON for parsing type strings from metadata JSON

## [F004-T102] Iceberg Metadata Parser

**description**: Implement parsing of Iceberg `metadata.json` files to extract table schema, snapshot history, partition specs, and the current snapshot ID.

**details**:
* Create `extension/iceberg/include/metadata/iceberg_metadata.h` and `extension/iceberg/src/metadata/iceberg_metadata.cpp`
* Define data structures:
  - `IcebergTableMetadata`: format_version, table_uuid, location, schema, current_snapshot_id, snapshots vector, partition_specs
  - `IcebergSnapshot`: snapshot_id (int64), timestamp_ms (int64), manifest_list (string), summary (map)
  - `IcebergField`: id, name, required (bool), type (string)
* Implement `IcebergTableMetadata parseMetadataJson(const std::string& json_content)` using RapidJSON
* Implement `std::string findLatestMetadataFile(const std::string& table_path, FileSystem* fs)`:
  - Glob `table_path + "/metadata/v*.metadata.json"`
  - Sort by version number (extract N from filename)
  - Return path of highest version
* Error handling: throw descriptive errors for malformed JSON, missing required fields (`format-version`, `schema`, `current-snapshot-id`)

## [F004-T103] Iceberg Manifest Parser

**description**: Implement Avro-based parsing of Iceberg manifest list files and manifest files to extract data file entries and delete file entries.

**details**:
* Create `extension/iceberg/include/metadata/iceberg_manifest.h` and `extension/iceberg/src/metadata/iceberg_manifest.cpp`
* Define data structures:
  - `ManifestListEntry`: manifest_path, manifest_length, partition_spec_id, content (0=data, 1=deletes), added_snapshot_id, partitions (vector of partition summaries)
  - `ManifestFileEntry`: status (0=existing, 1=added, 2=deleted), data_file (DataFileEntry)
  - `DataFileEntry`: file_path, file_format, record_count, file_size_in_bytes, partition (map), column_sizes, lower_bounds, upper_bounds
* Implement `std::vector<ManifestListEntry> parseManifestList(const std::string& avro_path, FileSystem* fs)`:
  - Use Arrow's Avro reader or a lightweight Avro library to parse the manifest list
  - Alternative: since manifest lists are also readable as Parquet in some Iceberg implementations, support both
* Implement `std::vector<ManifestFileEntry> parseManifestFile(const std::string& avro_path, FileSystem* fs)`
* Filter logic: only include entries with `status == 0` (existing) or `status == 1` (added)
* Separate data manifests (`content == 0`) from delete manifests (`content == 1`)

## [F004-T104] Iceberg Snapshot Resolver

**description**: Implement snapshot resolution logic that selects the correct snapshot based on user options (snapshot ID, timestamp, or current) and produces the final list of data/delete file paths.

**details**:
* Create `extension/iceberg/include/metadata/iceberg_snapshot.h` and `extension/iceberg/src/metadata/iceberg_snapshot.cpp`
* Implement `IcebergSnapshot resolveSnapshot(const IcebergTableMetadata& metadata, const IcebergScanOptions& options)`:
  - If `options.snapshot_id` is set: linear scan `metadata.snapshots` for matching `snapshot-id`; if not found → throw error listing available snapshot IDs
  - If `options.snapshot_ts` is set: find latest snapshot where `timestamp_ms <= parse(snapshot_ts)`; if none → throw error
  - Otherwise: use `metadata.current_snapshot_id`
* Implement `IcebergResolvedTable resolveTable(const std::string& table_path, const IcebergScanOptions& options, FileSystem* fs)`:
  - Call `findLatestMetadataFile()` → `parseMetadataJson()` → `resolveSnapshot()`
  - Read manifest list → parse each manifest file
  - Return struct with: schema, data_file_paths (vector<string>), delete_file_entries (vector<DeleteFileEntry>)
* This is the main entry point that chains metadata → snapshot → manifest list → manifest files → file paths

## [F004-T105] Iceberg Format Sniffer

**description**: Implement the Iceberg format auto-detection sniffer and schema inference from Iceberg metadata (not from individual Parquet files).

**details**:
* Create `extension/iceberg/include/iceberg_sniffer.h` and `extension/iceberg/src/iceberg_sniffer.cpp`
* Implement `IcebergSniffer` class (similar pattern to `ArrowSniffer`):
  - `static bool probe(const std::string& path, FileSystem* fs)`:
    - Construct `path + "/metadata/"`
    - List files, search for `v*.metadata.json` pattern
    - Return true if at least one match found
  - `EntrySchema sniff(const std::string& path, FileSystem* fs)`:
    - Call `findLatestMetadataFile()` → `parseMetadataJson()`
    - Extract schema fields → apply `mapIcebergSchema()` type mapping
    - Return `EntrySchema` (TableEntrySchema) with column names and mapped types
* Register the sniffer in the extension's `Init()` so it participates in format auto-detection
* Priority: Iceberg sniffer runs AFTER Parquet sniffer (`.parquet` extension check) but BEFORE CSV sniffer

## [F004-T106] Iceberg Options Builder

**description**: Implement `ArrowIcebergOptionsBuilder` extending `ArrowOptionsBuilder` that configures Arrow Parquet reading for Iceberg data files.

**details**:
* Create `extension/iceberg/include/iceberg_options.h` and `extension/iceberg/src/iceberg_options.cpp`
* Define `IcebergParseOptions` struct:
  - Inherits Parquet-related options (buffered_stream, pre_buffer, row_batch_size)
  - Adds Iceberg-specific context (columns marked for JSON serialization)
* Implement `ArrowIcebergOptionsBuilder` extending `ArrowOptionsBuilder`:
  - `build()` returns `ArrowOptions` with `ParquetFragmentScanOptions` and `ParquetFileFormat` (since Iceberg data files are Parquet)
  - Configure same Parquet reader properties as `ArrowParquetOptionsBuilder` (buffer size, threading, cache)
  - Handle `projectColumns()` and `skipRows()` (filter predicates) identically to the Parquet extension
* The key difference from `ArrowParquetOptionsBuilder`: the file paths come from Iceberg metadata resolution (not from user-provided glob), and column projection respects Iceberg field IDs

## [F004-T107] Iceberg Read Function

**description**: Implement the core `IcebergReadFunction` struct with `execFunc` and `sniffFunc` callbacks that integrates with NeuG's `LOAD FROM` pipeline.

**details**:
* Create `extension/iceberg/include/iceberg_read_function.h` and `extension/iceberg/src/iceberg_read_function.cpp`
* Define `IcebergReadFunction` struct following `ParquetReadFunction` pattern:
  - `static constexpr const char* name = "ICEBERG_SCAN"`
  - `static void execFunc(ReadSharedState& state, ReadLocalState& localState, ExecutionContext& ctx)`:
    1. Parse `IcebergScanOptions` from `state.schema.file.options`
    2. Resolve filesystem via VFS registry (`MetadataRegistry::getVFS().Provide(state.schema.file)`)
    3. Call `resolveTable()` to get data file paths from Iceberg metadata
    4. For each data file path: create `ArrowIcebergOptionsBuilder`, create `ArrowReader`, call `reader->read()`
    5. Handle delete files (call delete handling logic from T108)
    6. Handle nested type JSON serialization (from T110)
  - `static EntrySchema sniffFunc(FileSchema& fileSchema)`:
    1. Resolve filesystem via VFS
    2. Call `IcebergSniffer::sniff()` to infer schema from metadata
    3. Return `EntrySchema`
* Register as `TABLE_FUNCTION_ENTRY` via `ExtensionAPI::registerFunction<IcebergReadFunction>`
* Error handling: wrap all Arrow status errors with `THROW_IO_EXCEPTION` and add Iceberg-specific context

## [F004-T108] Delete File Handling

**description**: Implement Iceberg V2 delete file processing including positional deletes and equality deletes, ensuring deleted rows are excluded from query results.

**details**:
* Add delete handling logic within `extension/iceberg/src/iceberg_read_function.cpp` or as a separate utility
* Implement positional delete handling:
  - Read positional delete Parquet files (columns: `file_path`, `pos`)
  - Group by `file_path` → build `std::unordered_map<std::string, std::set<int64_t>>` of positions to skip
  - When reading each data file, maintain row counter and skip rows at deleted positions
  - Use sorted set + binary search for efficient position lookup
* Implement equality delete handling:
  - Read equality delete Parquet files (columns match the equality delete columns)
  - Build in-memory hash set of deleted value tuples
  - When reading data files, filter out rows matching any deleted tuple
* Edge cases:
  - Empty delete files → no-op, read all rows
  - Delete file references non-existent data file → ignore (data file may have been compacted)
  - Large positional delete sets → use bitmap for memory efficiency if >10K positions

## [F004-T109] Predicate Pushdown & Manifest Pruning

**description**: Implement manifest-level and data-file-level predicate pruning to skip irrelevant files before reading, and pass remaining predicates to Arrow's Parquet reader for row-group level filtering.

**details**:
* Add pruning logic in the metadata resolution pipeline (extend `resolveTable()` or create separate utility)
* Level 1 — Manifest-list pruning:
  - For each `ManifestListEntry`, check `partitions[]` summary (lower_bound, upper_bound)
  - Compare against query predicates (from `ReadSharedState.skipRows` expression)
  - Skip manifests where bounds prove no rows can match
* Level 2 — Data-file pruning:
  - For each `DataFileEntry`, check `lower_bounds`/`upper_bounds` per column
  - Use Iceberg field ID to match bounds to predicate columns
  - Skip data files where column bounds prove no rows can match
* Level 3 — Parquet row-group pruning:
  - Translate remaining predicates to `arrow::compute::Expression`
  - Pass to `ArrowIcebergOptionsBuilder::skipRows()` which sets filter on Arrow scanner
  - Arrow handles row-group statistics filtering internally
* Supported predicate types for pruning: `=`, `<`, `>`, `<=`, `>=`, `!=`, `IN`, `IS NULL`, `IS NOT NULL`
* Unsupported predicates → skip pruning (don't fail, just read the file)

## [F004-T110] Nested Type JSON Serialization

**description**: Implement post-processing of Arrow struct/list/map columns to JSON string representation before returning results to NeuG.

**details**:
* Add serialization utility in `extension/iceberg/src/iceberg_read_function.cpp` or as a separate helper
* During schema mapping (T101), mark columns with nested types (`struct`, `list`, `map`) as needing JSON serialization
* After reading a `RecordBatch` from Arrow, for each marked column:
  - Use `arrow::PrettyPrint` or manual traversal to convert `StructArray`/`ListArray`/`MapArray` to JSON strings
  - Replace the column in the batch with a `StringArray` containing JSON representations
* JSON format examples:
  - `STRUCT<name: STRING, score: DOUBLE>` → `'{"name":"Alice","score":95.5}'`
  - `LIST<INT>` → `'[1, 2, 3]'`
  - `MAP<STRING, INT>` → `'{"key1":1,"key2":2}'`
* Handle nulls: nested column value is null → produce null string (not "null" literal)
* Use RapidJSON `StringBuffer` + `Writer` for efficient JSON serialization
