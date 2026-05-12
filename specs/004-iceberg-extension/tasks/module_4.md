# Module 4: REST Catalog Integration (P4 — Future)

**Goal**: Enable users to query Iceberg tables by logical name via an external REST Catalog service, rather than specifying physical paths. This module is deferred to a future release.

**Assignee**: 
**Label**: iceberg, future
**Milestone**: 
**Project**: 

## [F004-T401] REST Catalog Client

**description**: Implement an HTTP client that resolves logical Iceberg table names to physical metadata locations via the Iceberg REST Catalog API protocol.

**details**:
* Create `extension/iceberg/include/catalog/iceberg_rest_catalog.h` and `extension/iceberg/src/catalog/iceberg_rest_catalog.cpp`
* Implement `IcebergRestCatalog` class:
  - Constructor takes `catalog_uri` (string) and optional auth token
  - `std::string resolveTableLocation(const std::string& table_name, const std::string& warehouse)`:
    1. Send GET request to `{catalog_uri}/v1/namespaces/{namespace}/tables/{table}` 
    2. Parse JSON response to extract `metadata-location` field
    3. Return the physical metadata file path (e.g., `s3://bucket/warehouse/table/metadata/v3.metadata.json`)
  - Handle HTTP errors: 404 → "Table not found in catalog", 401/403 → "Authentication failed", 5xx → "Catalog service unavailable"
* HTTP client options:
  - Use `httplib.h` (already in `third_party/httplib/`) for HTTP requests
  - Or use libcurl (already a dependency of the S3 extension)
* Authentication: support Bearer token via `Authorization` header
* Iceberg REST Catalog API spec reference: https://iceberg.apache.org/spec/#rest-catalog

## [F004-T402] Catalog Configuration Options Parsing

**description**: Parse catalog-related inline options (`CATALOG_TYPE`, `CATALOG_URI`, `WAREHOUSE`) from `LOAD FROM` syntax and route table resolution through the appropriate catalog implementation.

**details**:
* Extend `IcebergScanOptions` struct to include catalog fields:
  ```cpp
  struct IcebergScanOptions {
      // ... existing fields ...
      std::optional<std::string> catalog_type;    // CATALOG_TYPE='rest'
      std::optional<std::string> catalog_uri;     // CATALOG_URI='https://...'
      std::optional<std::string> warehouse;       // WAREHOUSE='s3://...'
  };
  ```
* Modify `IcebergReadFunction::execFunc()` routing logic:
  - If `catalog_type` is set:
    1. Validate required params (catalog_uri must be set for 'rest' type)
    2. Create `IcebergRestCatalog` with catalog_uri
    3. Call `resolveTableLocation(table_name, warehouse)` to get metadata path
    4. Continue with normal metadata resolution flow using resolved path
  - If `catalog_type` is NOT set:
    - Use the path directly (existing behavior from P1)
* Error handling:
  - `CATALOG_TYPE` set to unsupported value → error listing supported types ("rest")
  - `CATALOG_URI` missing when `CATALOG_TYPE='rest'` → descriptive error
  - `WAREHOUSE` is a scoping parameter sent TO the catalog (not a client-side path); it tells the catalog which namespace/location to search in
