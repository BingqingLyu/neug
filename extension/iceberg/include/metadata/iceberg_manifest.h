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

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include <arrow/filesystem/filesystem.h>

namespace neug {
namespace iceberg {

/**
 * @brief Summary of a single partition field within a manifest.
 *
 * Each element in ManifestListEntry::partitions corresponds to one
 * partition field in the partition spec.  Bounds are Iceberg
 * single-value-serialized binary (little-endian for numerics, UTF-8
 * for strings).
 */
struct PartitionFieldSummary {
  bool contains_null = false;
  bool contains_nan = false;
  std::string lower_bound;  // Iceberg binary-encoded
  std::string upper_bound;  // Iceberg binary-encoded
};

/**
 * @brief Entry from an Iceberg manifest list file.
 */
struct ManifestListEntry {
  std::string manifest_path;
  int64_t manifest_length = 0;
  int32_t partition_spec_id = 0;
  int32_t content = 0;  // 0 = data, 1 = deletes
  int64_t added_snapshot_id = 0;
  int32_t added_data_files_count = 0;
  int32_t existing_data_files_count = 0;
  int32_t deleted_data_files_count = 0;
  std::vector<PartitionFieldSummary> partitions;
};

/**
 * @brief Describes a single data/delete file within a manifest.
 */
struct DataFileEntry {
  std::string file_path;
  std::string file_format;  // "PARQUET"
  int32_t content = 0;      // 0=data, 1=position_deletes, 2=equality_deletes
  int64_t record_count = 0;
  int64_t file_size_in_bytes = 0;
  std::map<std::string, std::string> partition;
  std::map<int32_t, std::string> lower_bounds;
  std::map<int32_t, std::string> upper_bounds;
};

/**
 * @brief Entry from an Iceberg manifest file.
 */
struct ManifestFileEntry {
  int32_t status = 0;  // 0 = existing, 1 = added, 2 = deleted
  DataFileEntry data_file;
};

/**
 * @brief Parse a manifest list Avro file, returning entries for each manifest.
 *
 * Uses Arrow's Parquet reader as a fallback since Iceberg manifest lists
 * written by some engines are also readable as Parquet.
 *
 * @param manifest_list_path Full path to the manifest list file.
 * @param arrow_fs Arrow filesystem for reading.
 * @return Vector of manifest list entries.
 */
std::vector<ManifestListEntry> parseManifestList(
    const std::string& manifest_list_path,
    arrow::fs::FileSystem* arrow_fs);

/**
 * @brief Parse a manifest file, returning data/delete file entries.
 *
 * @param manifest_path Full path to the manifest file.
 * @param arrow_fs Arrow filesystem for reading.
 * @return Vector of manifest file entries (only active: status 0 or 1).
 */
std::vector<ManifestFileEntry> parseManifestFile(
    const std::string& manifest_path,
    arrow::fs::FileSystem* arrow_fs);

}  // namespace iceberg
}  // namespace neug
