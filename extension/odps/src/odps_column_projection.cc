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

#include "odps_column_projection.h"

#include <algorithm>
#include <unordered_set>

#include "neug/utils/exception/exception.h"

namespace neug {
namespace extension {
namespace odps {

OdpsColumnProjection buildColumnProjection(
    const std::vector<std::string>& schemaColumns,
    const std::vector<std::string>& projectColumns,
    const std::shared_ptr<::common::Expression>& filter) {
  std::unordered_set<std::string> required;
  if (projectColumns.empty()) {
    // No projection info -> every column is required (fall back to a full
    // read, preserving correctness).
    required.insert(schemaColumns.begin(), schemaColumns.end());
  } else {
    required.insert(projectColumns.begin(), projectColumns.end());
  }

  // Add every column the engine-side filter touches. The filter is always
  // re-applied after decoding (pushdown is best-effort -- T303/T304), so a
  // column referenced only by the predicate must still be materialized even
  // when it is not projected. Mirrors carquet's buildPhysicalProjection walk.
  std::vector<const ::common::Expression*> pending{filter.get()};
  while (!pending.empty()) {
    const auto* expression = pending.back();
    pending.pop_back();
    if (expression == nullptr) {
      continue;
    }
    for (const auto& operation : expression->operators()) {
      switch (operation.item_case()) {
      case ::common::ExprOpr::kVar:
        if (!operation.var().tag().has_name() ||
            operation.var().has_property()) {
          THROW_INVALID_ARGUMENT_EXCEPTION(
              "ODPS_SCAN: table filter requires a column name without a graph "
              "property");
        }
        required.insert(operation.var().tag().name());
        break;
      case ::common::ExprOpr::kCase:
        for (const auto& branch : operation.case_().when_then_expressions()) {
          pending.push_back(&branch.when_expression());
          pending.push_back(&branch.then_result_expression());
        }
        pending.push_back(&operation.case_().else_result_expression());
        break;
      case ::common::ExprOpr::kScalarFunc:
        for (const auto& child : operation.scalar_func().parameters()) {
          pending.push_back(&child);
        }
        break;
      case ::common::ExprOpr::kUdfFunc:
        for (const auto& child : operation.udf_func().parameters()) {
          pending.push_back(&child);
        }
        break;
      case ::common::ExprOpr::kToTuple:
        for (const auto& child : operation.to_tuple().fields()) {
          pending.push_back(&child);
        }
        break;
      case ::common::ExprOpr::kToList:
        for (const auto& child : operation.to_list().fields()) {
          pending.push_back(&child);
        }
        break;
      case ::common::ExprOpr::kToArray:
        for (const auto& child : operation.to_array().fields()) {
          pending.push_back(&child);
        }
        break;
      default:
        break;
      }
    }
  }

  // Emit the required columns in schema order. Requesting them in schema order
  // makes the read session's output order unambiguous (see the header): the
  // requested order and the table-schema order coincide, so this list matches
  // the layout of the returned Arrow batches.
  OdpsColumnProjection projection;
  for (const std::string& column : schemaColumns) {
    if (required.erase(column) > 0) {
      projection.columns.push_back(column);
    }
  }
  if (!required.empty()) {
    // A projected/filtered column that the table does not have. Report it the
    // same way carquet reports missing Parquet columns.
    std::vector<std::string> missing(required.begin(), required.end());
    std::sort(missing.begin(), missing.end());
    std::string message = "ODPS_SCAN: columns not found in the table schema:";
    for (size_t i = 0; i < missing.size(); ++i) {
      message += (i == 0 ? " " : ", ") + missing[i];
    }
    THROW_INVALID_ARGUMENT_EXCEPTION(message);
  }
  projection.isFullSchema = projection.columns.size() == schemaColumns.size();
  return projection;
}

}  // namespace odps
}  // namespace extension
}  // namespace neug
