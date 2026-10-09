/** Copyright 2020 Alibaba Group Holding Limited.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * 	http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#pragma once

#include <string>
#include <vector>

#include "neug/utils/io/read/common/options.h"
#include "neug/utils/io/read/common/schema.h"

namespace neug {
namespace extension {
namespace odps {

// Canonical option keys recognized on the odps data source (case-insensitive,
// resolved from `reader::FileSchema.options`).
struct OdpsConfigOptionKeys {
  static constexpr const char* kProject = "project";
  static constexpr const char* kSchema = "schema";
  static constexpr const char* kTable = "table";
  static constexpr const char* kPartitions = "partitions";
  static constexpr const char* kSplitSizeMb = "split_size_mb";
  static constexpr const char* kQuotaName = "quota_name";
};

// Schema name assumed when the address omits it (MaxCompute default schema).
inline constexpr const char* kOdpsDefaultSchema = "default";

// URL scheme handled by this extension.
inline constexpr const char* kOdpsScheme = "odps://";

/**
 * @brief Parsed, credential-free description of an ODPS source table.
 *
 * Produced from `odps://[project.][schema.]table[?pt=1/ds=x]` plus the
 * COPY/LOAD read knobs. Access credentials and the service endpoint are
 * intentionally NOT part of this descriptor so they never leak into query
 * text — they are resolved separately by `OdpsConnectionOptions` (T104).
 */
struct OdpsSourceDesc {
  // Empty means "not specified in the address"; the reader falls back to the
  // connection default project (T104) and errors if that is also empty.
  std::string project;
  // Defaults to `kOdpsDefaultSchema` when the address omits the schema segment.
  std::string schema = kOdpsDefaultSchema;
  // Required.
  std::string table;
  // Partition specs to prune to, each a complete '/'-joined partition path
  // (e.g. "pt=1/ds=x"). Maps 1:1 onto FilterOptions.mRequiredPartitions; empty
  // means "read all partitions".
  std::vector<std::string> partitions;
  // Split size hint in MiB; 0 means "unset" -> SDK default split mode.
  long splitSizeMb = 0;
  // Storage API quota name; empty -> SDK default (pay-as-you-go).
  std::string quotaName;

  bool hasProject() const { return !project.empty(); }
  bool hasSplitSize() const { return splitSizeMb > 0; }
  bool hasQuota() const { return !quotaName.empty(); }
};

/**
 * @brief Parses odps:// addresses and FileSchema options into OdpsSourceDesc.
 *
 * Address grammar (spec FR-001/003/005):
 *   odps://[project.][schema.]table[?pt=1/ds=x,pt=2/ds=y]
 *   - at most three dot-separated segments: project, schema, table;
 *   - a missing schema defaults to "default";
 *   - a missing project is left empty (resolved from the connection later);
 *   - everything after `?` is a partition spec: ',' or '&' separates MULTIPLE
 *     partitions (all are read), '/' separates the LEVELS within one partition
 *     (matching the SDK's SetPartitionSpec dialect), and each level is a
 *     `key=value` pair with non-empty key and value. Several single-level terms
 *     with differing keys and no '/' (e.g. "pt=1,ds=x") are rejected, since
 *     that is MaxCompute's comma-separated DDL habit rather than a partition
 *     separator here.
 * Illegal input (missing table, more than three segments, malformed partition)
 * raises a located `THROW_INVALID_ARGUMENT_EXCEPTION`.
 */
class OdpsOptions {
 public:
  /**
   * @brief Parse a single `odps://...` address string.
   * @param address the raw URL (the `odps://` prefix is optional here; it is
   *        stripped case-insensitively when present).
   * @return populated OdpsSourceDesc (read knobs left at defaults).
   */
  static OdpsSourceDesc parseAddress(const std::string& address);

  /**
   * @brief Resolve the full descriptor from a FileSchema.
   *
   * The URL is taken from `schema.paths[0]`; explicit options
   * (project/schema/table/partitions/split_size_mb/quota_name) fill any field
   * the address omits. When there is no path at all, the table must come from
   * options or an error is raised.
   */
  static OdpsSourceDesc fromFileSchema(const reader::FileSchema& schema);
};

}  // namespace odps
}  // namespace extension
}  // namespace neug
