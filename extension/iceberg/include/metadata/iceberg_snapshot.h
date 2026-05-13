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
#include <optional>
#include <string>
#include <vector>

#include "metadata/iceberg_manifest.h"
#include "metadata/iceberg_metadata.h"
#include "neug/compiler/common/case_insensitive_map.h"
#include "neug/utils/file_sys/file_system.h"

namespace neug {
namespace iceberg {

/**
 * @brief User-facing scan options parsed from LOAD FROM inline parameters.
 */
struct IcebergScanOptions {
  std::optional<int64_t> snapshot_id;       // SNAPSHOT_ID=12345
  std::optional<std::string> snapshot_ts;   // SNAPSHOT_TIMESTAMP='...'
  bool format_explicit = false;             // true if user wrote format='iceberg'
};

/**
 * @brief Result of resolving an Iceberg table: schema + file lists.
 */
struct IcebergResolvedTable {
  IcebergTableMetadata metadata;
  IcebergSnapshot selected_snapshot;
  std::vector<DataFileEntry> data_files;
  std::vector<DataFileEntry> delete_files;
};

/**
 * @brief Resolve the target snapshot from metadata based on scan options.
 *
 * @param metadata Parsed Iceberg table metadata.
 * @param options User-provided scan options (snapshot_id or snapshot_ts).
 * @return The resolved IcebergSnapshot.
 * @throws Exception if snapshot is not found.
 */
IcebergSnapshot resolveSnapshot(const IcebergTableMetadata& metadata,
                                const IcebergScanOptions& options);

/**
 * @brief Fully resolve an Iceberg table: metadata → snapshot → manifests →
 * file lists.
 *
 * This is the main entry point for the read path. It chains:
 *   findLatestMetadataFile → parseMetadataJson → resolveSnapshot →
 *   parseManifestList → parseManifestFile → collect data/delete files
 *
 * @param table_path Root path of the Iceberg table.
 * @param options User-provided scan options.
 * @param fs FileSystem for I/O.
 * @return Resolved table with metadata, snapshot, data files, delete files.
 */
IcebergResolvedTable resolveTable(const std::string& table_path,
                                  const IcebergScanOptions& options,
                                  fsys::FileSystem* fs);

/**
 * @brief Parse IcebergScanOptions from a key-value options map.
 *
 * Recognized keys: SNAPSHOT_ID, SNAPSHOT_TIMESTAMP, format
 */
IcebergScanOptions parseScanOptions(
    const common::case_insensitive_map_t<std::string>& options);

}  // namespace iceberg
}  // namespace neug
