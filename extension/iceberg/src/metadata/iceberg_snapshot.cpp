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

#include "metadata/iceberg_snapshot.h"

#include <algorithm>
#include <string>

#include <glog/logging.h>

#include "iceberg_predicate_pruner.h"
#include "neug/utils/exception/exception.h"

namespace neug {
namespace iceberg {

IcebergSnapshot resolveSnapshot(const IcebergTableMetadata& metadata,
                                const IcebergScanOptions& options) {
  // Validate: snapshot_id and snapshot_ts are mutually exclusive
  if (options.snapshot_id.has_value() && options.snapshot_ts.has_value()) {
    THROW_EXCEPTION_WITH_FILE_LINE(
        "[iceberg] SNAPSHOT_ID and SNAPSHOT_TIMESTAMP are mutually exclusive. "
        "Please specify only one.");
  }

  if (metadata.snapshots.empty()) {
    THROW_EXCEPTION_WITH_FILE_LINE(
        "[iceberg] Table has no snapshots. The table may be empty or "
        "metadata is corrupted.");
  }

  // Case 1: User specified a snapshot ID
  if (options.snapshot_id.has_value()) {
    int64_t target_id = options.snapshot_id.value();
    for (const auto& snap : metadata.snapshots) {
      if (snap.snapshot_id == target_id) {
        return snap;
      }
    }
    std::string ids;
    for (const auto& s : metadata.snapshots)
      ids += (ids.empty() ? "" : ", ") + std::to_string(s.snapshot_id);
    THROW_EXCEPTION_WITH_FILE_LINE(
        "[iceberg] Snapshot ID " + std::to_string(target_id) +
        " not found. Available: [" + ids + "]");
  }

  // Case 2: User specified a timestamp
  if (options.snapshot_ts.has_value()) {
    // Parse ISO timestamp to epoch ms (simplified: expect ms directly or
    // ISO string) For now, we support numeric ms directly
    // TODO: add ISO-8601 string parsing
    int64_t target_ms = 0;
    try {
      target_ms = std::stoll(options.snapshot_ts.value());
    } catch (...) {
      THROW_EXCEPTION_WITH_FILE_LINE(
          "[iceberg] Invalid SNAPSHOT_TIMESTAMP value: '" +
          options.snapshot_ts.value() +
          "'. Expected epoch milliseconds (integer).");
    }

    // Find latest snapshot where timestamp_ms <= target
    const IcebergSnapshot* best = nullptr;
    for (const auto& snap : metadata.snapshots) {
      if (snap.timestamp_ms <= target_ms) {
        if (!best || snap.timestamp_ms > best->timestamp_ms) {
          best = &snap;
        }
      }
    }
    if (!best) {
      THROW_EXCEPTION_WITH_FILE_LINE(
          "[iceberg] No snapshot found at or before timestamp " +
          options.snapshot_ts.value() + " ms.");
    }
    return *best;
  }

  // Case 3: Use current snapshot
  for (const auto& snap : metadata.snapshots) {
    if (snap.snapshot_id == metadata.current_snapshot_id) {
      return snap;
    }
  }

  // Fallback: current_snapshot_id doesn't match — use latest
  return metadata.snapshots.back();
}

IcebergResolvedTable resolveTable(const std::string& table_path,
                                  const IcebergScanOptions& options,
                                  fsys::FileSystem* fs,
                                  const ::common::Expression* predicate) {
  IcebergResolvedTable result;

  // Step 1: Read table metadata
  result.metadata = readTableMetadata(table_path, fs);

  // Step 2: Resolve snapshot
  result.selected_snapshot = resolveSnapshot(result.metadata, options);

  // Step 3: Read manifest list
  auto arrow_fs = fs->toArrowFileSystem();
  std::string manifest_list_path = result.selected_snapshot.manifest_list;

  // Resolve relative path
  if (!manifest_list_path.empty() && manifest_list_path[0] != '/' &&
      manifest_list_path.find("://") == std::string::npos) {
    manifest_list_path = table_path + "/" + manifest_list_path;
  }

  auto manifest_list_entries =
      parseManifestList(manifest_list_path, arrow_fs.get());

  // ── Predicate pruning (optional) ─────────────────────────────────────
  std::unique_ptr<IcebergPredicatePruner> pruner;
  if (predicate) {
    pruner = std::make_unique<IcebergPredicatePruner>(
        *predicate,
        result.metadata.schema_fields,
        result.metadata.partition_spec);
  }

  // Level 1: manifest-list pruning
  if (pruner) {
    size_t before = manifest_list_entries.size();
    manifest_list_entries.erase(
        std::remove_if(manifest_list_entries.begin(),
                        manifest_list_entries.end(),
                        [&](const ManifestListEntry& e) {
                          return e.content == 0 &&
                                 pruner->canSkipManifest(e);
                        }),
        manifest_list_entries.end());
    size_t after = manifest_list_entries.size();
    if (before != after) {
      LOG(INFO) << "[iceberg] Level-1 pruning: skipped "
                << (before - after) << " of " << before << " manifests";
    }
  }

  // Step 4: Read each manifest file
  for (const auto& ml_entry : manifest_list_entries) {
    std::string manifest_path = ml_entry.manifest_path;
    // Resolve relative path
    if (!manifest_path.empty() && manifest_path[0] != '/' &&
        manifest_path.find("://") == std::string::npos) {
      manifest_path = table_path + "/" + manifest_path;
    }

    auto manifest_entries = parseManifestFile(manifest_path, arrow_fs.get());

    for (auto& entry : manifest_entries) {
      // Resolve data file path
      auto& file_path = entry.data_file.file_path;
      if (!file_path.empty() && file_path[0] != '/' &&
          file_path.find("://") == std::string::npos) {
        file_path = result.metadata.location + "/" + file_path;
      }

      if (ml_entry.content == 0) {
        // Level 2: data-file pruning (only for data files, not deletes)
        if (pruner && pruner->canSkipDataFile(entry.data_file)) {
          LOG(INFO) << "[iceberg] Level-2 pruning: skipped data file "
                    << file_path;
          continue;
        }
        result.data_files.push_back(std::move(entry.data_file));
      } else {
        // Delete manifest — never prune delete files
        result.delete_files.push_back(std::move(entry.data_file));
      }
    }
  }

  return result;
}

IcebergScanOptions parseScanOptions(
    const common::case_insensitive_map_t<std::string>& options) {
  IcebergScanOptions scan_options;

  auto it = options.find("SNAPSHOT_ID");
  if (it != options.end()) {
    try {
      scan_options.snapshot_id = std::stoll(it->second);
    } catch (...) {
      THROW_EXCEPTION_WITH_FILE_LINE(
          "[iceberg] Invalid SNAPSHOT_ID value: '" + it->second +
          "'. Expected integer.");
    }
  }

  it = options.find("SNAPSHOT_TIMESTAMP");
  if (it != options.end()) {
    scan_options.snapshot_ts = it->second;
  }

  it = options.find("format");
  if (it != options.end() && it->second == "iceberg") {
    scan_options.format_explicit = true;
  }

  return scan_options;
}

}  // namespace iceberg
}  // namespace neug
