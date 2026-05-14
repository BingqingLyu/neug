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

#include "metadata/iceberg_manifest.h"
#include "metadata/iceberg_metadata.h"

#include "neug/generated/proto/plan/common.pb.h"
#include "neug/generated/proto/plan/expr.pb.h"

namespace neug {
namespace iceberg {

/**
 * @brief Predicate-based pruning for Iceberg manifests and data files.
 *
 * Implements two pruning levels:
 *  - Level 1 (manifest pruning): skips entire manifest files whose
 *    partition summaries prove no matching rows exist.
 *  - Level 2 (data-file pruning): skips individual data files whose
 *    per-column lower/upper bounds prove no matching rows exist.
 *
 * The pruner parses a common::Expression into a simple predicate tree
 * and evaluates it against binary-encoded Iceberg bounds.
 */
class IcebergPredicatePruner {
 public:
  /**
   * @brief Construct a pruner from a query predicate and table metadata.
   *
   * @param predicate  The WHERE-clause expression.
   * @param schema     Schema fields (for field_id ↔ name ↔ type mapping).
   * @param partition  Partition spec (for identifying partition columns).
   */
  IcebergPredicatePruner(
      const ::common::Expression& predicate,
      const std::vector<IcebergField>& schema,
      const std::vector<IcebergPartitionField>& partition);

  /**
   * @brief Level 1: can this entire manifest be skipped?
   *
   * Checks whether the manifest's partition field summaries allow
   * ruling out all rows.  Only works for identity-transform partition
   * columns that appear in the predicate.
   */
  bool canSkipManifest(const ManifestListEntry& entry) const;

  /**
   * @brief Level 2: can this data file be skipped?
   *
   * Checks whether the file's per-column lower/upper bounds allow
   * ruling out all rows.
   */
  bool canSkipDataFile(const DataFileEntry& entry) const;

 private:
  // ---------- internal predicate tree ------------------------------------
  struct ExprNode {
    enum Kind {
      VARIABLE,
      CONSTANT,
      COMPARISON,
      LOGICAL_AND,
      LOGICAL_OR,
      UNSUPPORTED
    };
    Kind kind = UNSUPPORTED;
    // VARIABLE
    std::string column_name;
    // CONSTANT
    ::common::Value const_value;  // only set for CONSTANT nodes
    bool has_const_value = false;
    // COMPARISON
    ::common::Logical op;  // EQ, NE, LT, LE, GT, GE
    // AND / OR / COMPARISON children
    std::vector<ExprNode> children;
  };

  ExprNode parseExpression(const ::common::Expression& expr);

  // ---------- evaluation against bounds ----------------------------------
  /**
   * Evaluate whether the predicate tree allows skipping given bounds.
   * @param lower  lower_bounds map (field_id → binary bytes)
   * @param upper  upper_bounds map (field_id → binary bytes)
   * @return true  if no rows can match (file/manifest can be skipped)
   */
  bool canSkipWithBounds(
      const ExprNode& node,
      const std::map<int32_t, std::string>& lower,
      const std::map<int32_t, std::string>& upper) const;

  /**
   * Compare an Iceberg binary-encoded bound value against a protobuf
   * constant.  Returns <0 if bound < val, 0 if equal, >0 if bound > val.
   * Returns std::nullopt when comparison is impossible (type mismatch, etc.).
   */
  std::optional<int> compareBoundToValue(
      const std::string& bound_bytes,
      const ::common::Value& val,
      const std::string& iceberg_type) const;

  // ---------- metadata maps ----------------------------------------------
  std::map<std::string, int32_t> name_to_field_id_;   // column name → field_id
  std::map<int32_t, std::string> field_id_to_type_;    // field_id → iceberg type
  // partition column name → index in ManifestListEntry::partitions vector
  std::map<std::string, size_t> partition_col_to_idx_;
  // partition source_id → partition spec index
  std::map<int32_t, size_t> partition_source_to_idx_;

  ExprNode root_;
};

}  // namespace iceberg
}  // namespace neug
