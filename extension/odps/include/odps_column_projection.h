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

#include <memory>
#include <string>
#include <vector>

#include "neug/generated/proto/plan/expr.pb.h"

namespace neug {
namespace extension {
namespace odps {

/// The set of table columns a read must materialize: the projected output
/// columns plus every column the engine-side filter references, in table-schema
/// order. Mirrors parquet/carquet's `buildPhysicalProjection`.
///
/// Requesting the columns in schema order makes the ODPS read session's output
/// order unambiguous: whether the SDK honors the requested order or its own
/// table-schema order, the two coincide, so `columns` always matches the layout
/// of the returned Arrow batches. This list feeds both
/// `FilterOptions.mRequiredDataColumns` (server-side column pruning, T301) and
/// the positional `column_names` that `reader::filter_chunk` /
/// `reader::project_chunk` decode against.
struct OdpsColumnProjection {
  /// Columns to read, in schema order. When `isFullSchema` is true this equals
  /// the whole schema and pruning is a no-op.
  std::vector<std::string> columns;
  /// True when `columns` covers every schema column, i.e. nothing is pruned.
  /// The caller then leaves `FilterOptions.mRequiredDataColumns` unset so a
  /// full read behaves exactly as it did before column pruning existed.
  bool isFullSchema = false;
};

/// Compute the columns a read must materialize.
///
/// @param schemaColumns  every data column of the table, in schema order (the
///                       sniffed `EntrySchema::columnNames`; partition columns
///                       are excluded in v1 and never appear here).
/// @param projectColumns the columns the query outputs; empty means "all".
/// @param filter         the engine-side `skip_rows` predicate (may be null).
///                       Every column it references is added to the required
///                       set, because the filter is re-applied after decoding
///                       even when it was also pushed down (T303/T304).
///
/// Deliberately SDK-free (no ODPS/Arrow headers) so it compiles and is
/// unit-testable in the default `NEUG_WITH_ODPS_SDK=OFF` build.
///
/// @throws INVALID_ARGUMENT when `projectColumns` or `filter` names a column
///         absent from `schemaColumns`, or when `filter` references a graph
///         property rather than a flat column (a table scan has no properties)
///         -- matching carquet's behavior.
OdpsColumnProjection buildColumnProjection(
    const std::vector<std::string>& schemaColumns,
    const std::vector<std::string>& projectColumns,
    const std::shared_ptr<::common::Expression>& filter);

}  // namespace odps
}  // namespace extension
}  // namespace neug
