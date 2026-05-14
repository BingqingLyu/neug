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

#include "iceberg_delete_filter.h"

#include <arrow/compute/api.h>
#include <arrow/io/file.h>
#include <arrow/table.h>
#include <parquet/arrow/reader.h>

#include <algorithm>

#include "glog/logging.h"
#include "neug/utils/exception/exception.h"

namespace neug {
namespace iceberg {

namespace {

// Read a Parquet file into an Arrow Table.
std::shared_ptr<arrow::Table> readParquetFile(
    const std::string& path, arrow::fs::FileSystem* arrow_fs) {
  auto file_result = arrow_fs->OpenInputFile(path);
  if (!file_result.ok()) {
    THROW_IO_EXCEPTION("[iceberg] Failed to open delete file '" + path +
                       "': " + file_result.status().ToString());
  }
  auto file = file_result.ValueOrDie();

  std::unique_ptr<parquet::arrow::FileReader> reader;
  auto status = parquet::arrow::OpenFile(file, arrow::default_memory_pool(),
                                         &reader);
  if (!status.ok()) {
    THROW_IO_EXCEPTION("[iceberg] Failed to read delete file '" + path +
                       "': " + status.ToString());
  }

  std::shared_ptr<arrow::Table> table;
  status = reader->ReadTable(&table);
  if (!status.ok()) {
    THROW_IO_EXCEPTION("[iceberg] Failed to read delete table from '" + path +
                       "': " + status.ToString());
  }
  return table;
}

// Compare a value at a given position in a chunked array against a target.
// Returns true if values are equal.
bool compareChunkedArrayValue(
    const std::shared_ptr<arrow::ChunkedArray>& col, int64_t row_idx,
    const std::shared_ptr<arrow::ChunkedArray>& del_col,
    int64_t del_row_idx) {
  // Locate chunk and local index for data
  int64_t offset = 0;
  const arrow::Array* data_array = nullptr;
  int64_t data_local_idx = 0;
  for (int c = 0; c < col->num_chunks(); ++c) {
    auto chunk = col->chunk(c);
    if (row_idx < offset + chunk->length()) {
      data_array = chunk.get();
      data_local_idx = row_idx - offset;
      break;
    }
    offset += chunk->length();
  }
  if (!data_array) return false;

  // Locate chunk and local index for delete
  offset = 0;
  const arrow::Array* del_array = nullptr;
  int64_t del_local_idx = 0;
  for (int c = 0; c < del_col->num_chunks(); ++c) {
    auto chunk = del_col->chunk(c);
    if (del_row_idx < offset + chunk->length()) {
      del_array = chunk.get();
      del_local_idx = del_row_idx - offset;
      break;
    }
    offset += chunk->length();
  }
  if (!del_array) return false;

  // Handle nulls
  if (data_array->IsNull(data_local_idx) && del_array->IsNull(del_local_idx)) {
    return true;  // Both null = match
  }
  if (data_array->IsNull(data_local_idx) || del_array->IsNull(del_local_idx)) {
    return false;  // One null = no match
  }

  // Use Arrow's scalar comparison for type-safe comparison
  auto data_scalar = data_array->GetScalar(data_local_idx);
  auto del_scalar = del_array->GetScalar(del_local_idx);
  if (!data_scalar.ok() || !del_scalar.ok()) return false;

  return data_scalar.ValueOrDie()->Equals(*del_scalar.ValueOrDie());
}

}  // namespace

void IcebergDeleteFilter::loadDeleteFiles(
    const std::vector<DataFileEntry>& delete_files,
    const std::string& table_path, arrow::fs::FileSystem* arrow_fs) {
  for (const auto& df : delete_files) {
    std::string path = df.file_path;
    // Resolve relative paths
    if (!path.empty() && path[0] != '/' &&
        path.find("://") == std::string::npos) {
      path = table_path + "/" + path;
    }

    if (df.content == 1) {
      // Positional delete
      loadPositionalDeleteFile(path, arrow_fs);
    } else if (df.content == 2) {
      // Equality delete
      loadEqualityDeleteFile(path, arrow_fs);
    } else {
      LOG(WARNING) << "[iceberg] Unknown delete file content type: "
                   << df.content << " for file: " << path;
    }
  }

  // Sort positional deletes for efficient lookup
  for (auto& [file_path, positions] : positional_deletes_) {
    std::sort(positions.begin(), positions.end());
  }
}

bool IcebergDeleteFilter::hasDeletes() const {
  return !positional_deletes_.empty() || !equality_delete_tables_.empty();
}

bool IcebergDeleteFilter::hasPositionalDeletes() const {
  return !positional_deletes_.empty();
}

bool IcebergDeleteFilter::hasEqualityDeletes() const {
  return !equality_delete_tables_.empty();
}

const std::vector<int64_t>* IcebergDeleteFilter::getPositionalDeletes(
    const std::string& data_file_path) const {
  auto it = positional_deletes_.find(data_file_path);
  if (it != positional_deletes_.end()) {
    return &it->second;
  }
  return nullptr;
}

void IcebergDeleteFilter::loadPositionalDeleteFile(
    const std::string& file_path, arrow::fs::FileSystem* arrow_fs) {
  auto table = readParquetFile(file_path, arrow_fs);

  // Positional delete files have schema: file_path (string), pos (int64)
  auto path_col = table->GetColumnByName("file_path");
  auto pos_col = table->GetColumnByName("pos");
  if (!path_col || !pos_col) {
    THROW_IO_EXCEPTION(
        "[iceberg] Positional delete file missing required columns "
        "'file_path' or 'pos': " +
        file_path);
  }

  int64_t num_rows = table->num_rows();
  for (int64_t i = 0; i < num_rows; ++i) {
    // Get file_path value
    int64_t offset = 0;
    std::string data_path;
    for (int c = 0; c < path_col->num_chunks(); ++c) {
      auto chunk = path_col->chunk(c);
      if (i < offset + chunk->length()) {
        auto str_array = std::static_pointer_cast<arrow::StringArray>(chunk);
        data_path = str_array->GetString(i - offset);
        break;
      }
      offset += chunk->length();
    }

    // Get pos value
    offset = 0;
    int64_t pos = -1;
    for (int c = 0; c < pos_col->num_chunks(); ++c) {
      auto chunk = pos_col->chunk(c);
      if (i < offset + chunk->length()) {
        auto int_array = std::static_pointer_cast<arrow::Int64Array>(chunk);
        pos = int_array->Value(i - offset);
        break;
      }
      offset += chunk->length();
    }

    if (!data_path.empty() && pos >= 0) {
      positional_deletes_[data_path].push_back(pos);
    }
  }
}

void IcebergDeleteFilter::loadEqualityDeleteFile(
    const std::string& file_path, arrow::fs::FileSystem* arrow_fs) {
  auto table = readParquetFile(file_path, arrow_fs);
  if (table->num_rows() > 0 && table->num_columns() > 0) {
    equality_delete_tables_.push_back(table);
  }
}

std::shared_ptr<arrow::Table> IcebergDeleteFilter::applyDeletes(
    const std::shared_ptr<arrow::Table>& data_table,
    const std::string& data_file_path) const {
  if (!hasDeletes()) {
    return data_table;
  }

  int64_t num_rows = data_table->num_rows();
  if (num_rows == 0) {
    return data_table;
  }

  // Build keep mask (true = keep the row)
  std::vector<bool> keep_mask(num_rows, true);

  // Apply positional deletes
  applyPositionalDeleteMask(num_rows, data_file_path, keep_mask);

  // Apply equality deletes
  applyEqualityDeleteMask(data_table, keep_mask);

  // Count kept rows
  int64_t keep_count = 0;
  for (bool k : keep_mask) {
    if (k) ++keep_count;
  }

  // If no rows deleted, return original
  if (keep_count == num_rows) {
    return data_table;
  }

  // If all rows deleted, return empty table with same schema
  if (keep_count == 0) {
    return data_table->Slice(0, 0);
  }

  // Build filter indices array for Arrow compute Take
  arrow::Int64Builder indices_builder;
  auto reserve_status = indices_builder.Reserve(keep_count);
  if (!reserve_status.ok()) {
    THROW_IO_EXCEPTION("[iceberg] Failed to reserve filter indices: " +
                       reserve_status.ToString());
  }

  for (int64_t i = 0; i < num_rows; ++i) {
    if (keep_mask[i]) {
      indices_builder.UnsafeAppend(i);
    }
  }

  std::shared_ptr<arrow::Array> indices;
  auto finish_status = indices_builder.Finish(&indices);
  if (!finish_status.ok()) {
    THROW_IO_EXCEPTION("[iceberg] Failed to build filter indices: " +
                       finish_status.ToString());
  }

  // Use Arrow's Take function to filter the table
  auto take_result = arrow::compute::Take(data_table, indices);
  if (!take_result.ok()) {
    THROW_IO_EXCEPTION("[iceberg] Failed to apply delete filter: " +
                       take_result.status().ToString());
  }

  return take_result.ValueOrDie().table();
}

void IcebergDeleteFilter::applyPositionalDeleteMask(
    int64_t num_rows, const std::string& data_file_path,
    std::vector<bool>& keep_mask) const {
  const auto* positions = getPositionalDeletes(data_file_path);
  if (!positions) return;

  for (int64_t pos : *positions) {
    if (pos >= 0 && pos < num_rows) {
      keep_mask[pos] = false;
    }
  }
}

void IcebergDeleteFilter::applyEqualityDeleteMask(
    const std::shared_ptr<arrow::Table>& data_table,
    std::vector<bool>& keep_mask) const {
  if (equality_delete_tables_.empty()) return;

  int64_t num_rows = data_table->num_rows();

  for (const auto& del_table : equality_delete_tables_) {
    // Get the columns in the delete file
    auto del_schema = del_table->schema();
    int del_num_cols = del_schema->num_fields();
    int64_t del_num_rows = del_table->num_rows();

    // Map delete columns to data columns by name
    std::vector<std::pair<std::shared_ptr<arrow::ChunkedArray>,
                          std::shared_ptr<arrow::ChunkedArray>>>
        col_pairs;  // (data_col, del_col)

    for (int dc = 0; dc < del_num_cols; ++dc) {
      auto del_field_name = del_schema->field(dc)->name();
      auto data_col = data_table->GetColumnByName(del_field_name);
      if (!data_col) {
        // Delete column not found in data — skip this delete table
        col_pairs.clear();
        break;
      }
      col_pairs.emplace_back(data_col, del_table->column(dc));
    }

    if (col_pairs.empty()) continue;

    // For each data row, check if it matches any delete row
    for (int64_t data_row = 0; data_row < num_rows; ++data_row) {
      if (!keep_mask[data_row]) continue;  // Already deleted

      for (int64_t del_row = 0; del_row < del_num_rows; ++del_row) {
        bool all_match = true;
        for (const auto& [data_col, del_col] : col_pairs) {
          if (!compareChunkedArrayValue(data_col, data_row, del_col,
                                        del_row)) {
            all_match = false;
            break;
          }
        }
        if (all_match) {
          keep_mask[data_row] = false;
          break;  // No need to check more delete rows
        }
      }
    }
  }
}

}  // namespace iceberg
}  // namespace neug
