# Feature Specification: Iceberg Data Lake Extension

**Feature Branch**: `004-iceberg-extension`  
**Created**: 2026-05-12  
**Status**: Draft  
**Input**: User description: "Support Apache Iceberg data lake through extension"

## Functional Modules *(mandatory)*

### Module 1: Iceberg Table Reading (Priority: P1)

**Purpose**: Enable users to query Iceberg tables directly through NeuG's `LOAD FROM` Cypher syntax. This is the core capability that bridges NeuG with data lake ecosystems, allowing seamless access to analytically curated datasets stored in Iceberg format.

**Why this priority**: Reading from Iceberg tables is the foundational capability — without it, no other Iceberg-related feature is useful. It unlocks immediate value by allowing graph queries to incorporate data lake datasets without data duplication or manual export.

**Independent Test**: Can be fully tested by loading the Iceberg extension and executing `LOAD FROM "iceberg_table_path"` queries against a pre-created Iceberg table, verifying that data is correctly returned with proper column names, types, and values.

**Key Components**:

1. **Iceberg Read Function**: A table function (similar to `PARQUET_SCAN`) registered via the Extension API that resolves Iceberg table metadata, identifies relevant data files, and reads them into NeuG's execution context.
2. **Iceberg Metadata Resolver**: Reads Iceberg table metadata (manifest list, manifest files) to determine which Parquet data files belong to the current snapshot, enabling snapshot-aware reads.
3. **Iceberg Schema Adapter**: Maps Iceberg schema types (including nested types like structs, lists, and maps) to NeuG's type system, ensuring correct data representation in query results.
4. **Iceberg Sniffer**: Infers schema from Iceberg table metadata (not from individual Parquet files), providing accurate column names and types for the `LOAD FROM` binding phase.

**Functional Requirements**:

1. **FR-001**: The extension MUST register an `ICEBERG_SCAN` table function that integrates with `LOAD FROM` syntax. The extension supports two invocation modes:
   - **Auto-detection** (preferred): `LOAD FROM "path/to/iceberg_table" RETURN *` — the system probes the path for a `metadata/` subdirectory containing `*.metadata.json` files; if found, it is treated as an Iceberg table automatically.
   - **Explicit format**: `LOAD FROM "path/to/iceberg_table" (format='iceberg') RETURN *` — the user explicitly specifies the format, useful when auto-detection is not possible or when the user wants to be explicit.
2. **FR-002**: The extension MUST read Iceberg table metadata (snapshot, manifest list, manifest files) to identify the correct Parquet data files for the current table snapshot.
3. **FR-003**: The extension MUST map Iceberg schema types to NeuG types as follows:
   - Direct mapping: `INT`→`INT32`, `LONG`→`INT64`, `FLOAT`→`FLOAT`, `DOUBLE`→`DOUBLE`, `DECIMAL(p,s)`→`DECIMAL`, `STRING`→`STRING`, `BOOLEAN`→`BOOL`, `DATE`→`DATE`, `TIMESTAMP`→`TIMESTAMP`/`TIMESTAMP_TZ`, `UUID`→`UUID`, `BINARY`→`BLOB`, `FIXED(L)`→`BLOB`
   - Fallback to STRING: `TIME`→`STRING` (NeuG has no native TIME type; stored as ISO format string e.g. `"14:30:00.000"`)
   - JSON stringification: `STRUCT`, `LIST`, `MAP`→`STRING` (serialized as JSON, consistent with existing nested type handling)
4. **FR-004**: The extension MUST support snapshot selection, allowing users to read a specific snapshot by ID or timestamp via inline options (e.g., `SNAPSHOT_ID=12345` or `SNAPSHOT_TIMESTAMP='2025-01-01T00:00:00'`).
5. **FR-005**: The extension MUST automatically infer the Iceberg table schema from metadata and make it available for the `LOAD FROM` binding phase, enabling column projection and type casting.
6. **FR-006**: The extension MUST support Iceberg column projection (reading only requested columns from Parquet data files), consistent with NeuG's existing column pruning behavior.
7. **FR-007**: The extension MUST support predicate pushdown by translating NeuG `WHERE` conditions into Iceberg/Parquet filter predicates where possible, reducing unnecessary data reads.
8. **FR-008**: The extension MUST handle Iceberg delete files (equality deletes and positional deletes) correctly, ensuring that deleted rows are excluded from query results.

**Acceptance Scenarios**:

1. **Given** an Iceberg table with columns `id (INT64)`, `name (STRING)`, `age (INT32)` stored on local filesystem, **When** user executes `LOAD FROM "path/to/iceberg_table" RETURN name, age`, **Then** the system auto-detects the Iceberg format via metadata directory probing, returns correct `name` and `age` values with proper types, and `id` column is not read (column projection).
2. **Given** an Iceberg table with 3 snapshots, **When** user executes `LOAD FROM "path/to/iceberg_table" (SNAPSHOT_ID=<specific_id>) RETURN *`, **Then** only data from the specified snapshot is returned.
3. **Given** an Iceberg table with equality delete files, **When** user executes `LOAD FROM "path/to/iceberg_table" RETURN *`, **Then** rows matching the delete predicates are excluded from results.
4. **Given** an Iceberg table with columns of various types (DATE, TIMESTAMP, DECIMAL, UUID), **When** user executes `LOAD FROM "path/to/iceberg_table" RETURN *`, **Then** all values are correctly mapped to NeuG types.
5. **Given** an Iceberg table with a `WHERE` filter condition on partition columns, **When** user executes `LOAD FROM "path/to/iceberg_table" WHERE partition_col='value' RETURN *`, **Then** the system prunes irrelevant data files at the manifest level before reading.

**Test Strategy**:

- **Unit Tests**: Iceberg schema type mapping (each Iceberg type → NeuG type); metadata parsing (snapshot, manifest list, manifest file); delete file handling logic.
- **Integration Tests**: End-to-end `LOAD FROM` queries against Iceberg tables with various schemas, snapshots, and delete files; column projection and filter pushdown verification.

---

### Module 2: Storage Backend Integration (Priority: P2)

**Purpose**: Enable Iceberg tables stored on remote storage backends (S3, OSS, etc.) to be queried seamlessly by leveraging NeuG's existing VFS (Virtual File System) and S3 extension infrastructure.

**Why this priority**: Most production Iceberg deployments store data on cloud object storage. Local filesystem support (P1) is essential for testing, but remote storage support is what makes this feature practical for real-world data lake use cases.

**Independent Test**: Can be fully tested by loading both the Iceberg and S3 extensions, then executing `LOAD FROM "s3://bucket/iceberg_table"` queries against Iceberg tables stored on S3-compatible storage.

**Key Components**:

1. **VFS-Compatible Path Resolution**: Leverage NeuG's existing VFS registry to resolve storage paths for Iceberg metadata and data files, enabling transparent access to local, S3, and OSS paths.

**Functional Requirements**:

1. **FR-009**: The extension MUST support Iceberg tables stored on local filesystem paths without additional configuration.
2. **FR-010**: The extension MUST support Iceberg tables stored on S3/OSS paths when the S3 extension is also loaded, using the VFS registry to resolve `s3://` and `oss://` paths for both metadata and data files.
3. **FR-011**: The extension MUST resolve Iceberg metadata file paths (metadata/json, manifest lists, manifest files) relative to the table's warehouse location, using the configured VFS for storage access.

**Acceptance Scenarios**:

1. **Given** an Iceberg table stored at `s3://my-bucket/warehouse/my_table/`, **When** user loads both S3 and Iceberg extensions and executes `LOAD FROM "s3://my-bucket/warehouse/my_table" RETURN *`, **Then** the system auto-detects the Iceberg format and correctly returns data from S3-stored Iceberg table.
2. **Given** an Iceberg table on OSS at `oss://my-bucket/warehouse/my_table/`, **When** user loads S3 and Iceberg extensions with appropriate OSS credentials, and executes `LOAD FROM "oss://my-bucket/warehouse/my_table" RETURN *`, **Then** the system correctly reads the Iceberg table from OSS.

**Test Strategy**:

- **Unit Tests**: Path resolution for different storage schemes (local, s3://, oss://).
- **Integration Tests**: End-to-end queries against Iceberg tables on S3/OSS (with S3 extension loaded).

---

### Module 3: Iceberg Extension Lifecycle & Installation (Priority: P3)

**Purpose**: Provide the standard NeuG extension lifecycle (INSTALL, LOAD, Name, Init) for the Iceberg extension, following the same pattern as existing extensions (JSON, Parquet, S3). This ensures consistent user experience and proper integration with NeuG's extension management system.

**Why this priority**: The extension lifecycle infrastructure is necessary but largely boilerplate — it follows the well-established pattern of existing extensions and is a prerequisite for deployment, but it doesn't deliver unique functional value beyond what the extension framework already provides.

**Independent Test**: Can be fully tested by executing `INSTALL ICEBERG;` and `LOAD ICEBERG;` commands and verifying that the extension registers correctly, the `ICEBERG_SCAN` function appears in the catalog, and subsequent queries work.

**Key Components**:

1. **Extension Entry Points**: `Init()`, `Name()`, `Install()`, and `Load()` functions following the standard NeuG extension contract.
2. **Extension Registration**: Registration of the `ICEBERG_SCAN` table function and extension metadata (name, description) via `ExtensionAPI::registerFunction` and `ExtensionAPI::registerExtension`.
3. **Build Configuration**: CMake build configuration for the Iceberg extension shared library, including dependency management for the Iceberg C++ library (or Arrow Iceberg integration).

**Functional Requirements**:

1. **FR-013**: The extension MUST expose `Init()` and `Name()` entry points conforming to the NeuG extension contract, with `Name()` returning `"ICEBERG"`.
2. **FR-014**: The extension MUST register itself via `ExtensionAPI::registerExtension` with name `"iceberg"` and a descriptive string upon `Init()`.
3. **FR-015**: The extension MUST be loadable via `INSTALL ICEBERG;` and `LOAD ICEBERG;` commands, following the standard NeuG extension lifecycle.
4. **FR-016**: The extension MUST be built as a separate shared library (`neug_iceberg_extension`) that is dynamically loaded via `dlopen`, consistent with existing extension build patterns.

**Acceptance Scenarios**:

1. **Given** a NeuG instance, **When** user executes `INSTALL ICEBERG;`, **Then** the Iceberg extension shared library is downloaded/built and placed in the user extension directory.
2. **Given** the Iceberg extension is installed, **When** user executes `LOAD ICEBERG;`, **Then** the `ICEBERG_SCAN` function is registered and available for `LOAD FROM` queries.
3. **Given** the Iceberg extension is loaded, **When** user queries `CALL show_extensions()`, **Then** the `"iceberg"` extension appears with its description.

**Test Strategy**:

- **Unit Tests**: Extension entry point behavior (Init registers functions, Name returns correct identifier).
- **Integration Tests**: Full INSTALL/LOAD lifecycle; extension discovery after loading.

---

### Module 4: REST Catalog Integration (Priority: P4 — Future)

**Purpose**: Enable users to query Iceberg tables by logical name via an external REST Catalog service, rather than specifying the full physical path. This supports enterprise environments where tables are managed centrally and discovered through a catalog service.

**Why this priority**: REST Catalog support adds convenience for enterprise deployments but requires additional HTTP protocol handling, authentication logic, and catalog API implementation. Direct path access (P1+P2) already covers the majority of use cases. Hive Metastore and Glue catalog are deferred further.

**Independent Test**: Can be tested by executing `LOAD FROM "my_table" (CATALOG_TYPE='rest', CATALOG_URI='https://...', WAREHOUSE='s3://...') RETURN *` against a running REST catalog service.

**Key Components**:

1. **REST Catalog Client**: HTTP client that implements the Iceberg REST Catalog API protocol to resolve logical table names to physical metadata locations.
2. **Catalog Configuration Options**: Inline options (`CATALOG_TYPE`, `CATALOG_URI`, `WAREHOUSE`) for specifying catalog connection parameters.

**Functional Requirements**:

1. **FR-017**: The extension MUST support REST catalog by resolving logical table names to metadata locations via the Iceberg REST Catalog API.
2. **FR-018**: The extension MUST allow users to specify catalog connection parameters (`CATALOG_TYPE='rest'`, `CATALOG_URI`, `WAREHOUSE`) via inline options in `LOAD FROM`.

**Acceptance Scenarios**:

1. **Given** an Iceberg REST catalog at `https://catalog.example.com`, **When** user executes `LOAD FROM "my_table" (CATALOG_TYPE='rest', CATALOG_URI='https://catalog.example.com', WAREHOUSE='s3://my-bucket/warehouse') RETURN *`, **Then** the extension resolves table metadata from the REST catalog and reads data correctly.

**Test Strategy**:

- **Unit Tests**: REST catalog API response parsing; catalog configuration validation.
- **Integration Tests**: End-to-end queries via REST catalog (requires a test catalog service).

---

### Edge Cases

- What happens when the Iceberg table has no data files (empty table)? The extension should return an empty result set with the correct schema inferred from metadata.
- What happens when the Iceberg table metadata references data files that don't exist or are inaccessible? The extension should report a clear error indicating which files are missing and suggest checking storage permissions.
- What happens when the Iceberg table uses nested types (structs, lists, maps)? The extension converts nested columns to **JSON string representation** (e.g., `STRUCT<name: STRING, score: DOUBLE>` → `'{"name":"Alice","score":95.5}'`). This is consistent with NeuG's existing Vertex/Edge/Path export behavior and avoids the current limitation where LOAD FROM cannot cast struct types to NeuG primitive types.
- What happens when the Iceberg table's snapshot is corrupted or the manifest list cannot be parsed? The extension should fail with a descriptive error and not silently return partial data.
- What happens when the user specifies an invalid `SNAPSHOT_ID`? The extension should report an error listing available snapshot IDs for reference.
- What happens when both S3 and Iceberg extensions are loaded but S3 credentials are missing? The extension should propagate the S3 credential error from the VFS layer with context about which Iceberg file failed to access.
- What happens when Iceberg data files use unsupported Parquet compression codecs? The extension should report an error identifying the unsupported codec and suggest installing the appropriate codec support.

## Assumptions

1. Iceberg data files are stored in Parquet format (the default and most common Iceberg file format). Other file formats (AVRO, ORC) are not supported in the initial release.
2. The extension relies on NeuG's existing Arrow-based reader infrastructure for reading Parquet data files, leveraging the same `ArrowReader` and `ArrowParquetOptionsBuilder` patterns used by the Parquet extension.
3. Iceberg metadata files (metadata/json, manifest lists, manifest files) are in their standard Avro/JSON formats as defined by the Iceberg spec.
4. The initial release targets Iceberg table format version 2 (V2), which includes support for delete files. V1 tables (without delete files) are also supported as a simpler case.
5. Users are expected to pre-create Iceberg tables using external tools (Spark, Flink, Trino, etc.) — the extension provides read-only access, not table creation or write capabilities.
6. The extension depends on the S3 extension for remote storage access; it does not bundle its own S3 client. Users must load the S3 extension separately if querying Iceberg tables on cloud storage.

## Success Criteria *(mandatory)*

### Measurable Outcomes

- **SC-001**: Users can successfully read data from a local Iceberg table with at least 10 columns and 1 million rows via `LOAD FROM` in under 10 seconds.
- **SC-002**: Column projection works correctly — querying 3 out of 10 columns reads only the required data, reducing I/O by at least 50% compared to reading all columns.
- **SC-003**: Snapshot selection works correctly — users can specify a snapshot ID or timestamp and receive data consistent with that specific snapshot.
- **SC-004**: Delete file handling works correctly — rows marked as deleted in Iceberg delete files are excluded from query results with 100% accuracy.
- **SC-005**: All Iceberg primitive types (INT, LONG, FLOAT, DOUBLE, STRING, BOOLEAN, DATE, TIMESTAMP, DECIMAL) are correctly mapped to NeuG types without data loss.
- **SC-006**: The extension loads and registers successfully via `INSTALL ICEBERG; LOAD ICEBERG;` within 5 seconds.
- **SC-007**: End-to-end query against an S3-stored Iceberg table (with S3 extension loaded) completes successfully and returns correct results.
- **SC-008**: The extension follows the same INSTALL/LOAD/LOAD FROM pattern as existing extensions (JSON, Parquet), requiring no additional learning curve for existing NeuG users.