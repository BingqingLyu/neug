/**
 * Copyright 2020 Alibaba Group Holding Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <gtest/gtest.h>

#include <arrow/api.h>
#include <arrow/filesystem/localfs.h>
#include <arrow/io/file.h>
#include <parquet/arrow/writer.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "metadata/iceberg_metadata.h"
#include "metadata/iceberg_manifest.h"
#include "metadata/iceberg_snapshot.h"
#include "schema/iceberg_type_mapper.h"
#include "iceberg_delete_filter.h"

namespace neug {
namespace test {

static constexpr const char* ICEBERG_TEST_DIR = "/tmp/iceberg_test";

// =============================================================================
// Test Suite 1: Iceberg Type Mapping
// =============================================================================

class IcebergTypeMapperTest : public ::testing::Test {};

TEST_F(IcebergTypeMapperTest, MapPrimitiveTypes) {
  EXPECT_EQ(iceberg::mapIcebergType("boolean"), common::LogicalTypeID::BOOL);
  EXPECT_EQ(iceberg::mapIcebergType("int"), common::LogicalTypeID::INT32);
  EXPECT_EQ(iceberg::mapIcebergType("long"), common::LogicalTypeID::INT64);
  EXPECT_EQ(iceberg::mapIcebergType("float"), common::LogicalTypeID::FLOAT);
  EXPECT_EQ(iceberg::mapIcebergType("double"), common::LogicalTypeID::DOUBLE);
  EXPECT_EQ(iceberg::mapIcebergType("string"), common::LogicalTypeID::STRING);
  EXPECT_EQ(iceberg::mapIcebergType("date"), common::LogicalTypeID::DATE);
  EXPECT_EQ(iceberg::mapIcebergType("uuid"), common::LogicalTypeID::UUID);
  EXPECT_EQ(iceberg::mapIcebergType("binary"), common::LogicalTypeID::BLOB);
}

TEST_F(IcebergTypeMapperTest, MapTimestampTypes) {
  EXPECT_EQ(iceberg::mapIcebergType("timestamp"),
            common::LogicalTypeID::TIMESTAMP);
  EXPECT_EQ(iceberg::mapIcebergType("timestamptz"),
            common::LogicalTypeID::TIMESTAMP_TZ);
  EXPECT_EQ(iceberg::mapIcebergType("time"), common::LogicalTypeID::STRING);
}

TEST_F(IcebergTypeMapperTest, MapParameterizedTypes) {
  // decimal(p,s) → DECIMAL
  EXPECT_EQ(iceberg::mapIcebergType("decimal(10,2)"),
            common::LogicalTypeID::DECIMAL);
  // fixed(L) → BLOB
  EXPECT_EQ(iceberg::mapIcebergType("fixed(16)"),
            common::LogicalTypeID::BLOB);
}

TEST_F(IcebergTypeMapperTest, MapNestedTypesToString) {
  // Nested types should map to STRING for JSON serialization
  EXPECT_EQ(iceberg::mapIcebergType("struct"),
            common::LogicalTypeID::STRING);
  EXPECT_EQ(iceberg::mapIcebergType("list"), common::LogicalTypeID::STRING);
  EXPECT_EQ(iceberg::mapIcebergType("map"), common::LogicalTypeID::STRING);
}

TEST_F(IcebergTypeMapperTest, CaseInsensitive) {
  EXPECT_EQ(iceberg::mapIcebergType("BOOLEAN"), common::LogicalTypeID::BOOL);
  EXPECT_EQ(iceberg::mapIcebergType("Long"), common::LogicalTypeID::INT64);
  EXPECT_EQ(iceberg::mapIcebergType("STRING"), common::LogicalTypeID::STRING);
}

TEST_F(IcebergTypeMapperTest, NeedsJsonSerialization) {
  EXPECT_TRUE(iceberg::needsJsonSerialization("struct"));
  EXPECT_TRUE(iceberg::needsJsonSerialization("list"));
  EXPECT_TRUE(iceberg::needsJsonSerialization("map"));
  EXPECT_FALSE(iceberg::needsJsonSerialization("long"));
  EXPECT_FALSE(iceberg::needsJsonSerialization("string"));
}

TEST_F(IcebergTypeMapperTest, MapSchema) {
  std::vector<std::pair<std::string, std::string>> fields = {
      {"id", "long"},
      {"name", "string"},
      {"score", "double"},
      {"tags", "list"},
  };

  auto columns = iceberg::mapIcebergSchema(fields);
  ASSERT_EQ(columns.size(), 4u);

  EXPECT_EQ(columns[0].name, "id");
  EXPECT_EQ(columns[0].type_id, common::LogicalTypeID::INT64);
  EXPECT_FALSE(columns[0].needs_json_serialization);

  EXPECT_EQ(columns[1].name, "name");
  EXPECT_EQ(columns[1].type_id, common::LogicalTypeID::STRING);
  EXPECT_FALSE(columns[1].needs_json_serialization);

  EXPECT_EQ(columns[2].name, "score");
  EXPECT_EQ(columns[2].type_id, common::LogicalTypeID::DOUBLE);
  EXPECT_FALSE(columns[2].needs_json_serialization);

  EXPECT_EQ(columns[3].name, "tags");
  EXPECT_EQ(columns[3].type_id, common::LogicalTypeID::STRING);
  EXPECT_TRUE(columns[3].needs_json_serialization);
}

// =============================================================================
// Test Suite 2: Metadata JSON Parsing
// =============================================================================

class IcebergMetadataTest : public ::testing::Test {};

TEST_F(IcebergMetadataTest, ParseValidMetadata) {
  std::string json = R"({
    "format-version": 1,
    "table-uuid": "test-uuid-001",
    "location": "/data/warehouse/test_table",
    "schema": {
      "type": "struct",
      "fields": [
        {"id": 1, "name": "id", "required": true, "type": "long"},
        {"id": 2, "name": "name", "required": false, "type": "string"},
        {"id": 3, "name": "value", "required": false, "type": "double"}
      ]
    },
    "current-snapshot-id": 12345,
    "snapshots": [
      {
        "snapshot-id": 12345,
        "timestamp-ms": 1700000000000,
        "manifest-list": "metadata/snap-12345-manifest-list.avro",
        "summary": {"operation": "append", "total-records": "100"}
      }
    ]
  })";

  auto metadata = iceberg::parseMetadataJson(json);

  EXPECT_EQ(metadata.format_version, 1);
  EXPECT_EQ(metadata.table_uuid, "test-uuid-001");
  EXPECT_EQ(metadata.location, "/data/warehouse/test_table");
  EXPECT_EQ(metadata.current_snapshot_id, 12345);

  // Schema fields
  ASSERT_EQ(metadata.schema_fields.size(), 3u);
  EXPECT_EQ(metadata.schema_fields[0].id, 1);
  EXPECT_EQ(metadata.schema_fields[0].name, "id");
  EXPECT_TRUE(metadata.schema_fields[0].required);
  EXPECT_EQ(metadata.schema_fields[0].type, "long");

  EXPECT_EQ(metadata.schema_fields[1].name, "name");
  EXPECT_EQ(metadata.schema_fields[1].type, "string");

  EXPECT_EQ(metadata.schema_fields[2].name, "value");
  EXPECT_EQ(metadata.schema_fields[2].type, "double");

  // Snapshots
  ASSERT_EQ(metadata.snapshots.size(), 1u);
  EXPECT_EQ(metadata.snapshots[0].snapshot_id, 12345);
  EXPECT_EQ(metadata.snapshots[0].timestamp_ms, 1700000000000);
  EXPECT_EQ(metadata.snapshots[0].manifest_list,
            "metadata/snap-12345-manifest-list.avro");
  EXPECT_EQ(metadata.snapshots[0].summary.at("operation"), "append");
  EXPECT_EQ(metadata.snapshots[0].summary.at("total-records"), "100");
}

TEST_F(IcebergMetadataTest, ParseEmptySnapshots) {
  std::string json = R"({
    "format-version": 2,
    "table-uuid": "empty-table",
    "location": "/data/empty",
    "schema": {"type": "struct", "fields": []},
    "current-snapshot-id": -1,
    "snapshots": []
  })";

  auto metadata = iceberg::parseMetadataJson(json);
  EXPECT_EQ(metadata.format_version, 2);
  EXPECT_EQ(metadata.current_snapshot_id, -1);
  EXPECT_TRUE(metadata.snapshots.empty());
  EXPECT_TRUE(metadata.schema_fields.empty());
}

TEST_F(IcebergMetadataTest, ParseInvalidJsonThrows) {
  EXPECT_THROW(iceberg::parseMetadataJson("not valid json"), std::exception);
}

TEST_F(IcebergMetadataTest, ParseMissingLocationThrows) {
  std::string json = R"({
    "format-version": 1,
    "schema": {"type": "struct", "fields": []}
  })";
  EXPECT_THROW(iceberg::parseMetadataJson(json), std::exception);
}

// =============================================================================
// Test Suite 3: Scan Options Parsing
// =============================================================================

class IcebergScanOptionsTest : public ::testing::Test {};

TEST_F(IcebergScanOptionsTest, ParseSnapshotId) {
  common::case_insensitive_map_t<std::string> options;
  options["SNAPSHOT_ID"] = "12345";

  auto scan_opts = iceberg::parseScanOptions(options);
  ASSERT_TRUE(scan_opts.snapshot_id.has_value());
  EXPECT_EQ(scan_opts.snapshot_id.value(), 12345);
  EXPECT_FALSE(scan_opts.snapshot_ts.has_value());
}

TEST_F(IcebergScanOptionsTest, ParseSnapshotTimestamp) {
  common::case_insensitive_map_t<std::string> options;
  options["SNAPSHOT_TIMESTAMP"] = "1700000000000";

  auto scan_opts = iceberg::parseScanOptions(options);
  EXPECT_FALSE(scan_opts.snapshot_id.has_value());
  ASSERT_TRUE(scan_opts.snapshot_ts.has_value());
  EXPECT_EQ(scan_opts.snapshot_ts.value(), "1700000000000");
}

TEST_F(IcebergScanOptionsTest, ParseFormatExplicit) {
  common::case_insensitive_map_t<std::string> options;
  options["format"] = "iceberg";

  auto scan_opts = iceberg::parseScanOptions(options);
  EXPECT_TRUE(scan_opts.format_explicit);
}

TEST_F(IcebergScanOptionsTest, EmptyOptions) {
  common::case_insensitive_map_t<std::string> options;
  auto scan_opts = iceberg::parseScanOptions(options);
  EXPECT_FALSE(scan_opts.snapshot_id.has_value());
  EXPECT_FALSE(scan_opts.snapshot_ts.has_value());
  EXPECT_FALSE(scan_opts.format_explicit);
}

// =============================================================================
// Test Suite 4: Snapshot Resolution
// =============================================================================

class IcebergSnapshotTest : public ::testing::Test {};

TEST_F(IcebergSnapshotTest, ResolveCurrentSnapshot) {
  iceberg::IcebergTableMetadata metadata;
  metadata.current_snapshot_id = 200;
  metadata.snapshots = {
      {100, 1700000000000, "manifest-list-1.avro", {}},
      {200, 1700000001000, "manifest-list-2.avro", {}},
  };

  iceberg::IcebergScanOptions options;  // no specific snapshot requested
  auto snapshot = iceberg::resolveSnapshot(metadata, options);
  EXPECT_EQ(snapshot.snapshot_id, 200);
}

TEST_F(IcebergSnapshotTest, ResolveBySnapshotId) {
  iceberg::IcebergTableMetadata metadata;
  metadata.current_snapshot_id = 200;
  metadata.snapshots = {
      {100, 1700000000000, "manifest-list-1.avro", {}},
      {200, 1700000001000, "manifest-list-2.avro", {}},
  };

  iceberg::IcebergScanOptions options;
  options.snapshot_id = 100;
  auto snapshot = iceberg::resolveSnapshot(metadata, options);
  EXPECT_EQ(snapshot.snapshot_id, 100);
  EXPECT_EQ(snapshot.manifest_list, "manifest-list-1.avro");
}

TEST_F(IcebergSnapshotTest, ResolveByTimestamp) {
  iceberg::IcebergTableMetadata metadata;
  metadata.current_snapshot_id = 300;
  metadata.snapshots = {
      {100, 1700000000000, "list-1.avro", {}},
      {200, 1700000001000, "list-2.avro", {}},
      {300, 1700000002000, "list-3.avro", {}},
  };

  // Ask for latest snapshot at or before timestamp 1700000001500
  iceberg::IcebergScanOptions options;
  options.snapshot_ts = "1700000001500";
  auto snapshot = iceberg::resolveSnapshot(metadata, options);
  EXPECT_EQ(snapshot.snapshot_id, 200);
}

TEST_F(IcebergSnapshotTest, ResolveNonexistentIdThrows) {
  iceberg::IcebergTableMetadata metadata;
  metadata.current_snapshot_id = 100;
  metadata.snapshots = {{100, 1700000000000, "list.avro", {}}};

  iceberg::IcebergScanOptions options;
  options.snapshot_id = 999;
  EXPECT_THROW(iceberg::resolveSnapshot(metadata, options), std::exception);
}

TEST_F(IcebergSnapshotTest, EmptySnapshotsThrows) {
  iceberg::IcebergTableMetadata metadata;
  metadata.current_snapshot_id = -1;

  iceberg::IcebergScanOptions options;
  EXPECT_THROW(iceberg::resolveSnapshot(metadata, options), std::exception);
}

TEST_F(IcebergSnapshotTest, MutuallyExclusiveOptionsThrows) {
  iceberg::IcebergTableMetadata metadata;
  metadata.current_snapshot_id = 100;
  metadata.snapshots = {{100, 1700000000000, "list.avro", {}}};

  iceberg::IcebergScanOptions options;
  options.snapshot_id = 100;
  options.snapshot_ts = "1700000000000";
  EXPECT_THROW(iceberg::resolveSnapshot(metadata, options), std::exception);
}

// =============================================================================
// Test Suite 5: Manifest Parsing (Integration)
// =============================================================================

class IcebergManifestTest : public ::testing::Test {
 public:
  void SetUp() override {
    if (std::filesystem::exists(ICEBERG_TEST_DIR)) {
      std::filesystem::remove_all(ICEBERG_TEST_DIR);
    }
    std::filesystem::create_directories(ICEBERG_TEST_DIR);
    arrow_fs_ = std::make_shared<arrow::fs::LocalFileSystem>();
  }

  void TearDown() override {
    if (std::filesystem::exists(ICEBERG_TEST_DIR)) {
      std::filesystem::remove_all(ICEBERG_TEST_DIR);
    }
  }

  void createManifestListParquet(const std::string& filename,
                                  const std::vector<std::string>& paths,
                                  const std::vector<int32_t>& contents) {
    auto schema = arrow::schema({
        arrow::field("manifest_path", arrow::utf8()),
        arrow::field("manifest_length", arrow::int64()),
        arrow::field("partition_spec_id", arrow::int32()),
        arrow::field("content", arrow::int32()),
        arrow::field("added_snapshot_id", arrow::int64()),
        arrow::field("added_data_files_count", arrow::int32()),
        arrow::field("existing_data_files_count", arrow::int32()),
        arrow::field("deleted_data_files_count", arrow::int32()),
    });

    arrow::StringBuilder path_builder;
    arrow::Int64Builder length_builder, snap_id_builder;
    arrow::Int32Builder spec_builder, content_builder, added_builder,
        existing_builder, deleted_builder;

    for (size_t i = 0; i < paths.size(); ++i) {
      ASSERT_TRUE(path_builder.Append(paths[i]).ok());
      ASSERT_TRUE(length_builder.Append(1024).ok());
      ASSERT_TRUE(spec_builder.Append(0).ok());
      ASSERT_TRUE(content_builder.Append(contents[i]).ok());
      ASSERT_TRUE(snap_id_builder.Append(1000).ok());
      ASSERT_TRUE(added_builder.Append(1).ok());
      ASSERT_TRUE(existing_builder.Append(0).ok());
      ASSERT_TRUE(deleted_builder.Append(0).ok());
    }

    std::shared_ptr<arrow::Array> arr_path, arr_length, arr_spec, arr_content,
        arr_snap, arr_added, arr_existing, arr_deleted;
    ASSERT_TRUE(path_builder.Finish(&arr_path).ok());
    ASSERT_TRUE(length_builder.Finish(&arr_length).ok());
    ASSERT_TRUE(spec_builder.Finish(&arr_spec).ok());
    ASSERT_TRUE(content_builder.Finish(&arr_content).ok());
    ASSERT_TRUE(snap_id_builder.Finish(&arr_snap).ok());
    ASSERT_TRUE(added_builder.Finish(&arr_added).ok());
    ASSERT_TRUE(existing_builder.Finish(&arr_existing).ok());
    ASSERT_TRUE(deleted_builder.Finish(&arr_deleted).ok());

    auto table = arrow::Table::Make(
        schema, {arr_path, arr_length, arr_spec, arr_content, arr_snap,
                 arr_added, arr_existing, arr_deleted});

    std::string filepath = std::string(ICEBERG_TEST_DIR) + "/" + filename;
    std::shared_ptr<arrow::io::FileOutputStream> outfile;
    PARQUET_ASSIGN_OR_THROW(outfile,
                            arrow::io::FileOutputStream::Open(filepath));
    PARQUET_THROW_NOT_OK(parquet::arrow::WriteTable(
        *table, arrow::default_memory_pool(), outfile, paths.size()));
  }

  void createManifestFileParquet(const std::string& filename,
                                  const std::vector<int32_t>& statuses,
                                  const std::vector<std::string>& file_paths,
                                  const std::vector<int64_t>& record_counts) {
    auto schema = arrow::schema({
        arrow::field("status", arrow::int32()),
        arrow::field("file_path", arrow::utf8()),
        arrow::field("file_format", arrow::utf8()),
        arrow::field("record_count", arrow::int64()),
        arrow::field("file_size_in_bytes", arrow::int64()),
    });

    arrow::Int32Builder status_builder;
    arrow::StringBuilder path_builder, format_builder;
    arrow::Int64Builder count_builder, size_builder;

    for (size_t i = 0; i < statuses.size(); ++i) {
      ASSERT_TRUE(status_builder.Append(statuses[i]).ok());
      ASSERT_TRUE(path_builder.Append(file_paths[i]).ok());
      ASSERT_TRUE(format_builder.Append("PARQUET").ok());
      ASSERT_TRUE(count_builder.Append(record_counts[i]).ok());
      ASSERT_TRUE(size_builder.Append(4096).ok());
    }

    std::shared_ptr<arrow::Array> arr_status, arr_path, arr_format, arr_count,
        arr_size;
    ASSERT_TRUE(status_builder.Finish(&arr_status).ok());
    ASSERT_TRUE(path_builder.Finish(&arr_path).ok());
    ASSERT_TRUE(format_builder.Finish(&arr_format).ok());
    ASSERT_TRUE(count_builder.Finish(&arr_count).ok());
    ASSERT_TRUE(size_builder.Finish(&arr_size).ok());

    auto table = arrow::Table::Make(
        schema, {arr_status, arr_path, arr_format, arr_count, arr_size});

    std::string filepath = std::string(ICEBERG_TEST_DIR) + "/" + filename;
    std::shared_ptr<arrow::io::FileOutputStream> outfile;
    PARQUET_ASSIGN_OR_THROW(outfile,
                            arrow::io::FileOutputStream::Open(filepath));
    PARQUET_THROW_NOT_OK(parquet::arrow::WriteTable(
        *table, arrow::default_memory_pool(), outfile, statuses.size()));
  }

 protected:
  std::shared_ptr<arrow::fs::LocalFileSystem> arrow_fs_;
};

TEST_F(IcebergManifestTest, ParseManifestList) {
  createManifestListParquet(
      "test-manifest-list.parquet",
      {"manifests/manifest-1.parquet", "manifests/manifest-2.parquet"},
      {0, 0});  // both are data manifests

  std::string path =
      std::string(ICEBERG_TEST_DIR) + "/test-manifest-list.parquet";
  auto entries = iceberg::parseManifestList(path, arrow_fs_.get());

  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0].manifest_path, "manifests/manifest-1.parquet");
  EXPECT_EQ(entries[0].content, 0);
  EXPECT_EQ(entries[1].manifest_path, "manifests/manifest-2.parquet");
  EXPECT_EQ(entries[1].content, 0);
}

TEST_F(IcebergManifestTest, ParseManifestListWithDeleteContent) {
  createManifestListParquet("test-manifest-mixed.parquet",
                            {"manifests/data.parquet", "manifests/delete.parquet"},
                            {0, 1});  // data + delete

  std::string path =
      std::string(ICEBERG_TEST_DIR) + "/test-manifest-mixed.parquet";
  auto entries = iceberg::parseManifestList(path, arrow_fs_.get());

  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0].content, 0);  // data
  EXPECT_EQ(entries[1].content, 1);  // delete
}

TEST_F(IcebergManifestTest, ParseManifestFile) {
  createManifestFileParquet(
      "test-manifest.parquet",
      {1, 0, 2},  // added, existing, deleted
      {"data/file1.parquet", "data/file2.parquet", "data/file3.parquet"},
      {100, 200, 50});

  std::string path =
      std::string(ICEBERG_TEST_DIR) + "/test-manifest.parquet";
  auto entries = iceberg::parseManifestFile(path, arrow_fs_.get());

  // Should skip deleted entries (status=2), keep added(1) and existing(0)
  ASSERT_EQ(entries.size(), 2u);
  EXPECT_EQ(entries[0].data_file.file_path, "data/file1.parquet");
  EXPECT_EQ(entries[0].data_file.record_count, 100);
  EXPECT_EQ(entries[1].data_file.file_path, "data/file2.parquet");
  EXPECT_EQ(entries[1].data_file.record_count, 200);
}

TEST_F(IcebergManifestTest, ParseManifestFileAllDeleted) {
  createManifestFileParquet("test-all-deleted.parquet",
                            {2, 2},  // both deleted
                            {"data/a.parquet", "data/b.parquet"}, {10, 20});

  std::string path =
      std::string(ICEBERG_TEST_DIR) + "/test-all-deleted.parquet";
  auto entries = iceberg::parseManifestFile(path, arrow_fs_.get());

  EXPECT_TRUE(entries.empty());
}

// =============================================================================
// Test Suite 6: Metadata File Discovery (requires filesystem)
// =============================================================================

class IcebergMetadataDiscoveryTest : public ::testing::Test {
 public:
  void SetUp() override {
    test_table_path_ = std::string(ICEBERG_TEST_DIR) + "/test_table";
    if (std::filesystem::exists(ICEBERG_TEST_DIR)) {
      std::filesystem::remove_all(ICEBERG_TEST_DIR);
    }
    std::filesystem::create_directories(test_table_path_ + "/metadata");
  }

  void TearDown() override {
    if (std::filesystem::exists(ICEBERG_TEST_DIR)) {
      std::filesystem::remove_all(ICEBERG_TEST_DIR);
    }
  }

  void writeMetadataFile(const std::string& filename,
                          const std::string& content) {
    std::string path = test_table_path_ + "/metadata/" + filename;
    std::ofstream ofs(path);
    ofs << content;
    ofs.close();
  }

 protected:
  std::string test_table_path_;
};

TEST_F(IcebergMetadataDiscoveryTest, FindLatestMetadataFile) {
  // Create multiple version files
  writeMetadataFile("v1.metadata.json", "{}");
  writeMetadataFile("v2.metadata.json", "{}");
  writeMetadataFile("v3.metadata.json", "{}");

  // Create a local filesystem implementation for testing
  class TestFileSystem : public fsys::FileSystem {
   public:
    std::vector<std::string> glob(const std::string& pattern) override {
      std::vector<std::string> results;
      // Extract directory from pattern
      auto slash_pos = pattern.rfind('/');
      std::string dir = pattern.substr(0, slash_pos);
      for (auto& entry : std::filesystem::directory_iterator(dir)) {
        auto name = entry.path().filename().string();
        // Simple glob: check if it matches v*.metadata.json
        if (name.size() > 14 && name[0] == 'v' &&
            name.find(".metadata.json") != std::string::npos) {
          results.push_back(entry.path().string());
        }
      }
      return results;
    }
    std::unique_ptr<arrow::fs::FileSystem> toArrowFileSystem() override {
      return std::make_unique<arrow::fs::LocalFileSystem>();
    }
  };

  TestFileSystem fs;
  auto result = iceberg::findLatestMetadataFile(test_table_path_, &fs);
  // Should find v3.metadata.json (highest version)
  EXPECT_TRUE(result.find("v3.metadata.json") != std::string::npos)
      << "Expected v3.metadata.json, got: " << result;
}

// =============================================================================
// IcebergDeleteFilterTest: Tests for delete file handling
// =============================================================================

class IcebergDeleteFilterTest : public ::testing::Test {
 protected:
  void SetUp() override {
    test_dir_ = std::filesystem::temp_directory_path() / "iceberg_delete_test";
    std::filesystem::create_directories(test_dir_);
    arrow_fs_ = std::make_shared<arrow::fs::LocalFileSystem>();
  }

  void TearDown() override {
    std::filesystem::remove_all(test_dir_);
  }

  // Write a Parquet file with given schema and data
  void writeParquetFile(const std::string& path,
                        const std::shared_ptr<arrow::Table>& table) {
    auto outfile = arrow::io::FileOutputStream::Open(path);
    ASSERT_TRUE(outfile.ok());
    auto status = parquet::arrow::WriteTable(
        *table, arrow::default_memory_pool(), outfile.ValueOrDie(), 1024);
    ASSERT_TRUE(status.ok());
  }

  // Create a positional delete file with (file_path, pos) columns
  std::string createPositionalDeleteFile(
      const std::string& name,
      const std::vector<std::pair<std::string, int64_t>>& deletes) {
    auto path_builder = std::make_shared<arrow::StringBuilder>();
    auto pos_builder = std::make_shared<arrow::Int64Builder>();

    for (const auto& [fp, pos] : deletes) {
      EXPECT_TRUE(path_builder->Append(fp).ok());
      EXPECT_TRUE(pos_builder->Append(pos).ok());
    }

    std::shared_ptr<arrow::Array> path_array, pos_array;
    EXPECT_TRUE(path_builder->Finish(&path_array).ok());
    EXPECT_TRUE(pos_builder->Finish(&pos_array).ok());

    auto schema = arrow::schema(
        {arrow::field("file_path", arrow::utf8()),
         arrow::field("pos", arrow::int64())});
    auto table = arrow::Table::Make(schema, {path_array, pos_array});

    std::string file_path = test_dir_ / name;
    writeParquetFile(file_path, table);
    return file_path;
  }

  // Create an equality delete file with given columns and values
  std::string createEqualityDeleteFile(
      const std::string& name,
      const std::shared_ptr<arrow::Table>& del_table) {
    std::string file_path = test_dir_ / name;
    writeParquetFile(file_path, del_table);
    return file_path;
  }

  // Create a data table for testing
  std::shared_ptr<arrow::Table> createDataTable() {
    auto id_builder = std::make_shared<arrow::Int64Builder>();
    auto name_builder = std::make_shared<arrow::StringBuilder>();
    auto age_builder = std::make_shared<arrow::Int32Builder>();

    // 5 rows: (1,Alice,30), (2,Bob,25), (3,Charlie,35), (4,David,28), (5,Eve,32)
    std::vector<int64_t> ids = {1, 2, 3, 4, 5};
    std::vector<std::string> names = {"Alice", "Bob", "Charlie", "David", "Eve"};
    std::vector<int32_t> ages = {30, 25, 35, 28, 32};

    for (size_t i = 0; i < ids.size(); ++i) {
      EXPECT_TRUE(id_builder->Append(ids[i]).ok());
      EXPECT_TRUE(name_builder->Append(names[i]).ok());
      EXPECT_TRUE(age_builder->Append(ages[i]).ok());
    }

    std::shared_ptr<arrow::Array> id_array, name_array, age_array;
    EXPECT_TRUE(id_builder->Finish(&id_array).ok());
    EXPECT_TRUE(name_builder->Finish(&name_array).ok());
    EXPECT_TRUE(age_builder->Finish(&age_array).ok());

    auto schema = arrow::schema(
        {arrow::field("id", arrow::int64()),
         arrow::field("name", arrow::utf8()),
         arrow::field("age", arrow::int32())});
    return arrow::Table::Make(schema, {id_array, name_array, age_array});
  }

  std::filesystem::path test_dir_;
  std::shared_ptr<arrow::fs::LocalFileSystem> arrow_fs_;
};

TEST_F(IcebergDeleteFilterTest, NoDeleteFiles) {
  iceberg::IcebergDeleteFilter filter;
  std::vector<iceberg::DataFileEntry> empty_deletes;
  filter.loadDeleteFiles(empty_deletes, test_dir_.string(), arrow_fs_.get());

  EXPECT_FALSE(filter.hasDeletes());
  EXPECT_FALSE(filter.hasPositionalDeletes());
  EXPECT_FALSE(filter.hasEqualityDeletes());

  auto data = createDataTable();
  auto result = filter.applyDeletes(data, "/some/data/file.parquet");
  EXPECT_EQ(result->num_rows(), 5);
}

TEST_F(IcebergDeleteFilterTest, PositionalDeletesSingleFile) {
  // Create positional delete: delete rows 1 and 3 from a data file
  std::string data_file_path = "/table/data/file1.parquet";
  auto del_path = createPositionalDeleteFile(
      "pos_delete.parquet",
      {{data_file_path, 1}, {data_file_path, 3}});

  iceberg::DataFileEntry del_entry;
  del_entry.file_path = del_path;
  del_entry.content = 1;  // positional delete

  iceberg::IcebergDeleteFilter filter;
  filter.loadDeleteFiles({del_entry}, test_dir_.string(), arrow_fs_.get());

  EXPECT_TRUE(filter.hasDeletes());
  EXPECT_TRUE(filter.hasPositionalDeletes());
  EXPECT_FALSE(filter.hasEqualityDeletes());

  // Check specific positions
  const auto* positions = filter.getPositionalDeletes(data_file_path);
  ASSERT_NE(positions, nullptr);
  EXPECT_EQ(positions->size(), 2);
  EXPECT_EQ((*positions)[0], 1);
  EXPECT_EQ((*positions)[1], 3);

  // Apply to data table
  auto data = createDataTable();
  auto result = filter.applyDeletes(data, data_file_path);
  EXPECT_EQ(result->num_rows(), 3);  // 5 - 2 = 3

  // Verify remaining rows: indices 0, 2, 4 → (1,Alice,30), (3,Charlie,35), (5,Eve,32)
  auto id_col = std::static_pointer_cast<arrow::Int64Array>(
      result->column(0)->chunk(0));
  EXPECT_EQ(id_col->Value(0), 1);
  EXPECT_EQ(id_col->Value(1), 3);
  EXPECT_EQ(id_col->Value(2), 5);
}

TEST_F(IcebergDeleteFilterTest, PositionalDeletesDifferentFiles) {
  // Deletes for file1 and file2
  std::string file1 = "/table/data/file1.parquet";
  std::string file2 = "/table/data/file2.parquet";
  auto del_path = createPositionalDeleteFile(
      "pos_delete_multi.parquet",
      {{file1, 0}, {file1, 4}, {file2, 2}});

  iceberg::DataFileEntry del_entry;
  del_entry.file_path = del_path;
  del_entry.content = 1;

  iceberg::IcebergDeleteFilter filter;
  filter.loadDeleteFiles({del_entry}, test_dir_.string(), arrow_fs_.get());

  // Check file1 positions
  const auto* pos1 = filter.getPositionalDeletes(file1);
  ASSERT_NE(pos1, nullptr);
  EXPECT_EQ(pos1->size(), 2);

  // Check file2 positions
  const auto* pos2 = filter.getPositionalDeletes(file2);
  ASSERT_NE(pos2, nullptr);
  EXPECT_EQ(pos2->size(), 1);

  // File not in deletes returns nullptr
  EXPECT_EQ(filter.getPositionalDeletes("/other/file.parquet"), nullptr);

  // Apply to file1 data
  auto data = createDataTable();
  auto result = filter.applyDeletes(data, file1);
  EXPECT_EQ(result->num_rows(), 3);  // deleted rows 0 and 4
}

TEST_F(IcebergDeleteFilterTest, EqualityDeleteSingleColumn) {
  // Create equality delete: delete rows where id=2 or id=4
  auto id_builder = std::make_shared<arrow::Int64Builder>();
  EXPECT_TRUE(id_builder->Append(2).ok());
  EXPECT_TRUE(id_builder->Append(4).ok());
  std::shared_ptr<arrow::Array> id_array;
  EXPECT_TRUE(id_builder->Finish(&id_array).ok());

  auto del_schema = arrow::schema({arrow::field("id", arrow::int64())});
  auto del_table = arrow::Table::Make(del_schema, {id_array});

  auto del_path = createEqualityDeleteFile("eq_delete.parquet", del_table);

  iceberg::DataFileEntry del_entry;
  del_entry.file_path = del_path;
  del_entry.content = 2;  // equality delete

  iceberg::IcebergDeleteFilter filter;
  filter.loadDeleteFiles({del_entry}, test_dir_.string(), arrow_fs_.get());

  EXPECT_TRUE(filter.hasDeletes());
  EXPECT_FALSE(filter.hasPositionalDeletes());
  EXPECT_TRUE(filter.hasEqualityDeletes());

  auto data = createDataTable();
  auto result = filter.applyDeletes(data, "/any/file.parquet");
  EXPECT_EQ(result->num_rows(), 3);  // 5 - 2 = 3

  // Remaining: id=1,3,5
  auto id_col = std::static_pointer_cast<arrow::Int64Array>(
      result->column(0)->chunk(0));
  EXPECT_EQ(id_col->Value(0), 1);
  EXPECT_EQ(id_col->Value(1), 3);
  EXPECT_EQ(id_col->Value(2), 5);
}

TEST_F(IcebergDeleteFilterTest, EqualityDeleteMultiColumn) {
  // Delete rows where (id=2, name="Bob")
  auto id_builder = std::make_shared<arrow::Int64Builder>();
  auto name_builder = std::make_shared<arrow::StringBuilder>();
  EXPECT_TRUE(id_builder->Append(2).ok());
  EXPECT_TRUE(name_builder->Append("Bob").ok());

  std::shared_ptr<arrow::Array> id_array, name_array;
  EXPECT_TRUE(id_builder->Finish(&id_array).ok());
  EXPECT_TRUE(name_builder->Finish(&name_array).ok());

  auto del_schema = arrow::schema(
      {arrow::field("id", arrow::int64()),
       arrow::field("name", arrow::utf8())});
  auto del_table = arrow::Table::Make(del_schema, {id_array, name_array});

  auto del_path = createEqualityDeleteFile("eq_multi.parquet", del_table);

  iceberg::DataFileEntry del_entry;
  del_entry.file_path = del_path;
  del_entry.content = 2;

  iceberg::IcebergDeleteFilter filter;
  filter.loadDeleteFiles({del_entry}, test_dir_.string(), arrow_fs_.get());

  auto data = createDataTable();
  auto result = filter.applyDeletes(data, "/any/file.parquet");
  EXPECT_EQ(result->num_rows(), 4);  // Only (2,Bob) deleted
}

TEST_F(IcebergDeleteFilterTest, CombinedPositionalAndEqualityDeletes) {
  std::string data_file_path = "/table/data/file1.parquet";

  // Positional delete: row 0
  auto pos_del_path = createPositionalDeleteFile(
      "pos_del.parquet", {{data_file_path, 0}});

  // Equality delete: id=5
  auto id_builder = std::make_shared<arrow::Int64Builder>();
  EXPECT_TRUE(id_builder->Append(5).ok());
  std::shared_ptr<arrow::Array> id_array;
  EXPECT_TRUE(id_builder->Finish(&id_array).ok());
  auto del_schema = arrow::schema({arrow::field("id", arrow::int64())});
  auto del_table = arrow::Table::Make(del_schema, {id_array});
  auto eq_del_path = createEqualityDeleteFile("eq_del.parquet", del_table);

  iceberg::DataFileEntry pos_entry;
  pos_entry.file_path = pos_del_path;
  pos_entry.content = 1;

  iceberg::DataFileEntry eq_entry;
  eq_entry.file_path = eq_del_path;
  eq_entry.content = 2;

  iceberg::IcebergDeleteFilter filter;
  filter.loadDeleteFiles({pos_entry, eq_entry}, test_dir_.string(),
                         arrow_fs_.get());

  EXPECT_TRUE(filter.hasPositionalDeletes());
  EXPECT_TRUE(filter.hasEqualityDeletes());

  auto data = createDataTable();
  auto result = filter.applyDeletes(data, data_file_path);
  EXPECT_EQ(result->num_rows(), 3);  // row 0 (positional) + id=5 (equality) = 2 deleted

  // Remaining: (2,Bob,25), (3,Charlie,35), (4,David,28)
  auto id_col = std::static_pointer_cast<arrow::Int64Array>(
      result->column(0)->chunk(0));
  EXPECT_EQ(id_col->Value(0), 2);
  EXPECT_EQ(id_col->Value(1), 3);
  EXPECT_EQ(id_col->Value(2), 4);
}

TEST_F(IcebergDeleteFilterTest, NoMatchingDeletesReturnOriginal) {
  // Positional deletes for a different file
  std::string data_file_path = "/table/data/file1.parquet";
  auto del_path = createPositionalDeleteFile(
      "pos_other.parquet",
      {{"/table/data/other_file.parquet", 0},
       {"/table/data/other_file.parquet", 1}});

  iceberg::DataFileEntry del_entry;
  del_entry.file_path = del_path;
  del_entry.content = 1;

  iceberg::IcebergDeleteFilter filter;
  filter.loadDeleteFiles({del_entry}, test_dir_.string(), arrow_fs_.get());

  auto data = createDataTable();
  auto result = filter.applyDeletes(data, data_file_path);
  // No deletes for this file — all 5 rows remain
  EXPECT_EQ(result->num_rows(), 5);
}

TEST_F(IcebergDeleteFilterTest, AllRowsDeleted) {
  std::string data_file_path = "/table/data/file1.parquet";
  auto del_path = createPositionalDeleteFile(
      "pos_all.parquet",
      {{data_file_path, 0}, {data_file_path, 1}, {data_file_path, 2},
       {data_file_path, 3}, {data_file_path, 4}});

  iceberg::DataFileEntry del_entry;
  del_entry.file_path = del_path;
  del_entry.content = 1;

  iceberg::IcebergDeleteFilter filter;
  filter.loadDeleteFiles({del_entry}, test_dir_.string(), arrow_fs_.get());

  auto data = createDataTable();
  auto result = filter.applyDeletes(data, data_file_path);
  EXPECT_EQ(result->num_rows(), 0);
  EXPECT_EQ(result->num_columns(), 3);  // Schema preserved
}

}  // namespace test
}  // namespace neug
