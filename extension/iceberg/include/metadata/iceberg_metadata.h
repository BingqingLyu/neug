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
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "neug/utils/file_sys/file_system.h"

namespace neug {
namespace iceberg {

/**
 * @brief Represents a single field in an Iceberg table schema.
 */
struct IcebergField {
  int32_t id;
  std::string name;
  bool required;
  std::string type;  // raw Iceberg type string (e.g., "long", "struct<...>")
};

/**
 * @brief Represents a snapshot in Iceberg table history.
 */
struct IcebergSnapshot {
  int64_t snapshot_id;
  int64_t timestamp_ms;
  std::string manifest_list;  // relative path to manifest-list file
  std::map<std::string, std::string> summary;
};

/**
 * @brief Represents the parsed content of an Iceberg metadata.json file.
 */
struct IcebergTableMetadata {
  int32_t format_version;
  std::string table_uuid;
  std::string location;  // table root location
  std::vector<IcebergField> schema_fields;
  int64_t current_snapshot_id;
  std::vector<IcebergSnapshot> snapshots;
};

/**
 * @brief Parse the content of an Iceberg metadata.json file.
 *
 * @param json_content The raw JSON string content of the metadata file.
 * @return Parsed IcebergTableMetadata struct.
 * @throws Exception on malformed JSON or missing required fields.
 */
IcebergTableMetadata parseMetadataJson(const std::string& json_content);

/**
 * @brief Find the latest metadata file in the given Iceberg table path.
 *
 * Globs for `table_path/metadata/v*.metadata.json` and returns the one
 * with the highest version number.
 *
 * @param table_path Root path of the Iceberg table.
 * @param fs FileSystem to use for file listing.
 * @return Full path to the latest metadata file.
 * @throws Exception if no metadata file is found.
 */
std::string findLatestMetadataFile(const std::string& table_path,
                                   fsys::FileSystem* fs);

/**
 * @brief Read and parse the metadata from an Iceberg table path.
 *
 * Convenience function that combines findLatestMetadataFile + read +
 * parseMetadataJson.
 *
 * @param table_path Root path of the Iceberg table.
 * @param fs FileSystem to use for file I/O.
 * @return Parsed IcebergTableMetadata.
 */
IcebergTableMetadata readTableMetadata(const std::string& table_path,
                                       fsys::FileSystem* fs);

}  // namespace iceberg
}  // namespace neug
