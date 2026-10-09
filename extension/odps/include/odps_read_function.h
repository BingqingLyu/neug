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
#include <utility>
#include <vector>

#include "neug/compiler/function/function.h"
#include "neug/compiler/function/read_function.h"
#include "neug/utils/exception/exception.h"
#include "neug/utils/io/read/common/row_expression_filter.h"
#include "odps_connection.h"
#include "odps_options.h"
#include "odps_record_batch_supplier.h"
#include "odps_schema_converter.h"

namespace neug {
namespace function {

/**
 * @brief ODPS/MaxCompute scan table function.
 *
 * Registered as `ODPS_SCAN` so that `LOAD FROM "odps://..." RETURN ...` and
 * `COPY tbl FROM (LOAD FROM ...)` resolve to this data source via the standard
 * `{FORMAT}_SCAN` convention. The three callbacks mirror the ReadFunction
 * contract (sniff schema, materialize into a Context, or stream as chunks).
 *
 * Module 1 wires these to the Storage API reader: `sniffFunc` maps the table
 * schema (T105), while `execFunc`/`supplierFunc` stream Arrow record batches
 * through `OdpsRecordBatchSupplier` and the C Data Interface bridge (T106).
 */
struct OdpsReadFunction {
  static constexpr const char* name = "ODPS_SCAN";

  static function_set getFunctionSet() {
    auto typeIDs =
        std::vector<::neug::DataTypeId>{::neug::DataTypeId::kVarchar};
    auto readFunction = std::make_unique<ReadFunction>(name, typeIDs);
    readFunction->execFunc = execFunc;
    readFunction->supplierFunc = supplierFunc;
    readFunction->sniffFunc = sniffFunc;
    function_set functionSet;
    functionSet.push_back(std::move(readFunction));
    return functionSet;
  }

  static execution::Context execFunc(
      std::shared_ptr<reader::ReadSharedState> state) {
    // Eager path (LOAD FROM ... RETURN). Stream every batch through the
    // supplier and materialize it into a Context. The supplier pushes column
    // pruning (T301), partition pruning (T302) and the filter predicate (T303)
    // down to the SDK, so a batch carries only the pruned projection in schema
    // order -- exactly what supplier->PhysicalColumnNames() reports. We decode
    // against that list (not the full schema) and then re-apply the engine-side
    // projection/filter contract every reader honors: filter_chunk enforces the
    // full predicate (pushdown is best-effort, T304) and project_chunk narrows
    // to the requested output columns. Both are no-ops for `RETURN *` with no
    // predicate, so ODPS behaves exactly like the CSV/parquet scans downstream.
    auto supplier =
        std::make_shared<extension::odps::OdpsRecordBatchSupplier>(state);
    const std::vector<std::string>& columnNames =
        supplier->PhysicalColumnNames();
    execution::Context ctx;
    while (auto chunk = supplier->GetNextChunk()) {
      auto filtered = reader::filter_chunk(*chunk, state->skipRows, columnNames,
                                           state->parameters);
      ctx.append_chunk(
          reader::project_chunk(filtered, columnNames, state->projectColumns));
    }
    return ctx;
  }

  static std::shared_ptr<IDataChunkSupplier> supplierFunc(
      std::shared_ptr<reader::ReadSharedState> state) {
    // Lazy path (COPY ... FROM (LOAD FROM ...) fusion). Emits full-table
    // columns in schema order; the COPY insert operator maps columns by index
    // downstream. Column pruning (T301) is therefore disabled here -- pruning
    // to a subset would shift the by-index mapping -- so the supplier reads
    // every column exactly as it did before T301.
    return std::make_shared<extension::odps::OdpsRecordBatchSupplier>(
        std::move(state), /*enableColumnPruning=*/false);
  }

  static std::shared_ptr<reader::EntrySchema> sniffFunc(
      const reader::FileSchema& schema) {
    // Resolve the source table (odps:// address + options) and the
    // credential/endpoint configuration, then read the authoritative
    // IODPSTableSchema through the ODPS core API and map it to a NeuG
    // EntrySchema. Without the SDK build this throws a clear "requires
    // NEUG_WITH_ODPS_SDK" error (see OdpsSchemaConverter::sniffTableSchema).
    auto source = extension::odps::OdpsOptions::fromFileSchema(schema);
    auto connectionOptions =
        extension::odps::OdpsConnectionOptionsBuilder(schema).build();
    extension::odps::OdpsConnection connection(connectionOptions);
    return extension::odps::OdpsSchemaConverter::sniffTableSchema(connection,
                                                                  source);
  }
};

}  // namespace function
}  // namespace neug
