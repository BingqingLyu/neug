# Module 2: Storage Backend Integration (P2)

**Goal**: Enable Iceberg tables stored on remote storage backends (S3, OSS) to be queried seamlessly by leveraging NeuG's existing VFS and S3 extension infrastructure.

**Assignee**: 
**Label**: iceberg
**Milestone**: 
**Project**: 

## [F004-T201] VFS-Compatible Path Resolution

**description**: Ensure all Iceberg metadata and data file reads go through NeuG's VFS registry, enabling transparent access to local, S3, and OSS storage without Iceberg-specific storage code.

**details**:
* Modify metadata resolution logic (from T102, T103, T104) to accept `FileSystem*` parameter and use it for all file I/O:
  - `findLatestMetadataFile()`: use `fs->glob()` instead of direct filesystem calls
  - `parseManifestList()` / `parseManifestFile()`: read Avro files via `fs->toArrowFileSystem()`
  - Data file reading in `execFunc()`: pass `fs->toArrowFileSystem()` to `ArrowReader`
* Implement path resolution for relative paths in Iceberg metadata:
  - Manifest list paths in snapshots are relative to table root → prepend `table_path + "/"`
  - Data file paths in manifests may be relative or absolute:
    - If starts with protocol scheme (`s3://`, `oss://`, `/`) → use as-is
    - Otherwise → prepend table `location` field from metadata
* Resolve VFS at the top level in `IcebergReadFunction::execFunc()`:
  - Extract protocol from table path (e.g., `s3://` → "s3", `/data/` → "local")
  - Call `MetadataRegistry::getVFS().Provide(state.schema.file)` to get appropriate `FileSystem`
  - Pass same FileSystem instance to all downstream metadata/data reads
* Ensure the extension does NOT import or link against S3-specific libraries — it only uses the `FileSystem` interface

## [F004-T202] S3/OSS Integration Verification

**description**: Verify end-to-end reading of Iceberg tables stored on S3/OSS when both Iceberg and S3 extensions are loaded, ensuring metadata and data file paths resolve correctly through VFS.

**details**:
* Create integration test (Python pytest) that:
  1. Loads both S3 and Iceberg extensions
  2. Queries an Iceberg table at `s3://test-bucket/warehouse/test_table`
  3. Verifies correct data is returned
* Test path resolution for S3-stored tables:
  - Table path: `s3://bucket/warehouse/my_table`
  - Metadata glob: `s3://bucket/warehouse/my_table/metadata/v*.metadata.json`
  - Manifest list: `s3://bucket/warehouse/my_table/metadata/snap-xxx-manifest-list.avro`
  - Data files: `s3://bucket/warehouse/my_table/data/00001.parquet` (relative) or `s3://other-bucket/data/00001.parquet` (absolute)
* Error scenario: S3 extension not loaded → user gets clear error message indicating S3 extension is required for `s3://` paths
* Error scenario: S3 credentials missing → error propagated from VFS layer with context about which Iceberg file (metadata/manifest/data) failed to access
* Note: This task requires a test S3-compatible environment (e.g., MinIO) or can be done with mocked VFS
