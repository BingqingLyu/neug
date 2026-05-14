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

#pragma once

#include <arrow/api.h>
#include <arrow/filesystem/filesystem.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "metadata/iceberg_manifest.h"

namespace neug {
namespace iceberg {

/**
 * @brief Handles Iceberg V2 delete files (positional and equality deletes).
 *
 * Iceberg V2 supports two types of delete files:
 * - Positional deletes: contain (file_path, pos) pairs identifying specific
 *   rows by their 0-based position within a data file.
 * - Equality deletes: contain column values; any data row matching all columns
 *   in a delete record is considered deleted.
 *
 * Usage:
 *   IcebergDeleteFilter filter;
 *   filter.loadDeleteFiles(delete_files, table_path, arrow_fs);
 *   auto filtered_table = filter.applyDeletes(data_table, data_file_path);
 */
class IcebergDeleteFilter {
 public:
  IcebergDeleteFilter() = default;

  /**
   * @brief Load and parse all delete files.
   *
   * Reads each delete file (Parquet format) and categorizes them as
   * positional or equality deletes based on DataFileEntry.content.
   *
   * @param delete_files List of delete file entries from manifest resolution.
   * @param table_path Root path of the Iceberg table (for relative path
   * resolution).
   * @param arrow_fs Arrow filesystem for reading delete files.
   */
  void loadDeleteFiles(const std::vector<DataFileEntry>& delete_files,
                       const std::string& table_path,
                       arrow::fs::FileSystem* arrow_fs);

  /** @return true if any delete files were loaded. */
  bool hasDeletes() const;

  /** @return true if positional delete files exist. */
  bool hasPositionalDeletes() const;

  /** @return true if equality delete files exist. */
  bool hasEqualityDeletes() const;

  /**
   * @brief Get deleted row positions for a specific data file.
   * @param data_file_path The data file path to look up.
   * @return Pointer to sorted vector of deleted positions, or nullptr if none.
   */
  const std::vector<int64_t>* getPositionalDeletes(
      const std::string& data_file_path) const;

  /**
   * @brief Apply all deletes (positional + equality) to a single data file's
   * Arrow Table.
   *
   * @param data_table The data read from a single data file.
   * @param data_file_path The path of the data file (for positional delete
   * matching).
   * @return A new Arrow Table with deleted rows removed.
   */
  std::shared_ptr<arrow::Table> applyDeletes(
      const std::shared_ptr<arrow::Table>& data_table,
      const std::string& data_file_path) const;

 private:
  /**
   * @brief Load a positional delete file.
   * Positional delete files have schema: file_path (string), pos (int64).
   */
  void loadPositionalDeleteFile(const std::string& file_path,
                                arrow::fs::FileSystem* arrow_fs);

  /**
   * @brief Load an equality delete file.
   * Equality delete files contain columns matching data schema columns.
   */
  void loadEqualityDeleteFile(const std::string& file_path,
                              arrow::fs::FileSystem* arrow_fs);

  /**
   * @brief Apply positional deletes to build a selection mask.
   * @param num_rows Total rows in the data table.
   * @param data_file_path The data file path for position lookup.
   * @param keep_mask Boolean vector (true = keep the row).
   */
  void applyPositionalDeleteMask(int64_t num_rows,
                                 const std::string& data_file_path,
                                 std::vector<bool>& keep_mask) const;

  /**
   * @brief Apply equality deletes to update a selection mask.
   * @param data_table The data table to check against.
   * @param keep_mask Boolean vector (true = keep the row).
   */
  void applyEqualityDeleteMask(const std::shared_ptr<arrow::Table>& data_table,
                               std::vector<bool>& keep_mask) const;

  // Positional deletes: file_path → sorted list of deleted positions
  std::unordered_map<std::string, std::vector<int64_t>> positional_deletes_;

  // Equality deletes: list of delete tables (each table's rows are delete keys)
  std::vector<std::shared_ptr<arrow::Table>> equality_delete_tables_;
};

}  // namespace iceberg
}  // namespace neug
