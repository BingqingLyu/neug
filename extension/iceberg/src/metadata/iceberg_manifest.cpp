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

#include "metadata/iceberg_manifest.h"

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <arrow/table.h>
#include <parquet/arrow/reader.h>

#include "neug/utils/exception/exception.h"

namespace neug {
namespace iceberg {

namespace {

// Helper: read an Avro/Parquet manifest file into an Arrow Table.
// Iceberg manifest files are Avro, but many implementations also write
// them in a way that's readable via Parquet. We try Parquet first.
std::shared_ptr<arrow::Table> readManifestAsTable(
    const std::string& path, arrow::fs::FileSystem* arrow_fs) {
  auto file_result = arrow_fs->OpenInputFile(path);
  if (!file_result.ok()) {
    THROW_IO_EXCEPTION("[iceberg] Failed to open manifest file '" + path +
                       "': " + file_result.status().ToString());
  }
  auto file = file_result.ValueOrDie();

  // Try reading as Parquet (common for Spark/Flink-generated Iceberg tables)
  std::unique_ptr<parquet::arrow::FileReader> reader;
  auto status = parquet::arrow::OpenFile(file, arrow::default_memory_pool(),
                                         &reader);
  if (!status.ok()) {
    THROW_IO_EXCEPTION("[iceberg] Failed to read manifest file '" + path +
                       "' as Parquet/Avro: " + status.ToString());
  }

  std::shared_ptr<arrow::Table> table;
  status = reader->ReadTable(&table);
  if (!status.ok()) {
    THROW_IO_EXCEPTION("[iceberg] Failed to read manifest table from '" +
                       path + "': " + status.ToString());
  }
  return table;
}

// Helper: get string value from a column at a given row
std::string getStringValue(const std::shared_ptr<arrow::Table>& table,
                           const std::string& col_name, int64_t row) {
  auto col = table->GetColumnByName(col_name);
  if (!col || col->num_chunks() == 0) return "";

  // Find the chunk and local index
  int64_t offset = 0;
  for (int i = 0; i < col->num_chunks(); ++i) {
    auto chunk = col->chunk(i);
    if (row < offset + chunk->length()) {
      int64_t local_idx = row - offset;
      if (chunk->IsNull(local_idx)) return "";
      auto str_array = std::static_pointer_cast<arrow::StringArray>(chunk);
      return str_array->GetString(local_idx);
    }
    offset += chunk->length();
  }
  return "";
}

// Helper: get int32 value from a column at a given row
int32_t getInt32Value(const std::shared_ptr<arrow::Table>& table,
                      const std::string& col_name, int64_t row,
                      int32_t default_val = 0) {
  auto col = table->GetColumnByName(col_name);
  if (!col || col->num_chunks() == 0) return default_val;

  int64_t offset = 0;
  for (int i = 0; i < col->num_chunks(); ++i) {
    auto chunk = col->chunk(i);
    if (row < offset + chunk->length()) {
      int64_t local_idx = row - offset;
      if (chunk->IsNull(local_idx)) return default_val;
      auto int_array = std::static_pointer_cast<arrow::Int32Array>(chunk);
      return int_array->Value(local_idx);
    }
    offset += chunk->length();
  }
  return default_val;
}

// Helper: get int64 value from a column at a given row
int64_t getInt64Value(const std::shared_ptr<arrow::Table>& table,
                      const std::string& col_name, int64_t row,
                      int64_t default_val = 0) {
  auto col = table->GetColumnByName(col_name);
  if (!col || col->num_chunks() == 0) return default_val;

  int64_t offset = 0;
  for (int i = 0; i < col->num_chunks(); ++i) {
    auto chunk = col->chunk(i);
    if (row < offset + chunk->length()) {
      int64_t local_idx = row - offset;
      if (chunk->IsNull(local_idx)) return default_val;
      auto int_array = std::static_pointer_cast<arrow::Int64Array>(chunk);
      return int_array->Value(local_idx);
    }
    offset += chunk->length();
  }
  return default_val;
}

}  // namespace

std::vector<ManifestListEntry> parseManifestList(
    const std::string& manifest_list_path,
    arrow::fs::FileSystem* arrow_fs) {
  auto table = readManifestAsTable(manifest_list_path, arrow_fs);
  std::vector<ManifestListEntry> entries;

  int64_t num_rows = table->num_rows();
  entries.reserve(num_rows);

  for (int64_t i = 0; i < num_rows; ++i) {
    ManifestListEntry entry;
    entry.manifest_path = getStringValue(table, "manifest_path", i);
    entry.manifest_length = getInt64Value(table, "manifest_length", i);
    entry.partition_spec_id = getInt32Value(table, "partition_spec_id", i);
    entry.content = getInt32Value(table, "content", i);
    entry.added_snapshot_id = getInt64Value(table, "added_snapshot_id", i);
    entry.added_data_files_count =
        getInt32Value(table, "added_data_files_count", i);
    entry.existing_data_files_count =
        getInt32Value(table, "existing_data_files_count", i);
    entry.deleted_data_files_count =
        getInt32Value(table, "deleted_data_files_count", i);
    entries.push_back(std::move(entry));
  }

  return entries;
}

std::vector<ManifestFileEntry> parseManifestFile(
    const std::string& manifest_path,
    arrow::fs::FileSystem* arrow_fs) {
  auto table = readManifestAsTable(manifest_path, arrow_fs);
  std::vector<ManifestFileEntry> entries;

  int64_t num_rows = table->num_rows();
  entries.reserve(num_rows);

  for (int64_t i = 0; i < num_rows; ++i) {
    ManifestFileEntry entry;
    entry.status = getInt32Value(table, "status", i);

    // Only include active entries (existing=0, added=1)
    if (entry.status == 2) continue;  // skip deleted

    // data_file is a nested struct column in Iceberg manifests.
    // The exact column name varies: "data_file" struct or flattened columns.
    // Try flattened approach first (common in Parquet representation):
    entry.data_file.file_path = getStringValue(table, "data_file.file_path", i);
    if (entry.data_file.file_path.empty()) {
      // Try alternative column naming
      entry.data_file.file_path = getStringValue(table, "file_path", i);
    }
    entry.data_file.file_format =
        getStringValue(table, "data_file.file_format", i);
    if (entry.data_file.file_format.empty()) {
      entry.data_file.file_format = getStringValue(table, "file_format", i);
    }
    // content: 0=data, 1=position_deletes, 2=equality_deletes
    entry.data_file.content =
        getInt32Value(table, "data_file.content", i);
    if (entry.data_file.content == 0) {
      entry.data_file.content = getInt32Value(table, "content", i);
    }
    entry.data_file.record_count =
        getInt64Value(table, "data_file.record_count", i);
    if (entry.data_file.record_count == 0) {
      entry.data_file.record_count = getInt64Value(table, "record_count", i);
    }
    entry.data_file.file_size_in_bytes =
        getInt64Value(table, "data_file.file_size_in_bytes", i);
    if (entry.data_file.file_size_in_bytes == 0) {
      entry.data_file.file_size_in_bytes =
          getInt64Value(table, "file_size_in_bytes", i);
    }

    entries.push_back(std::move(entry));
  }

  return entries;
}

}  // namespace iceberg
}  // namespace neug
