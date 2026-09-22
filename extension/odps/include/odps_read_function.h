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

#include "neug/compiler/function/function.h"
#include "neug/compiler/function/read_function.h"
#include "neug/utils/exception/exception.h"
#include "odps_connection.h"
#include "odps_options.h"
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
 * Module 1 (tasks T105/T106) wires these to the Storage API reader; the
 * callbacks below are placeholders that fail loudly until then.
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
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "ODPS_SCAN: reading ODPS tables is not yet implemented (module 1, "
        "tasks T105/T106 pending)");
  }

  static std::shared_ptr<IDataChunkSupplier> supplierFunc(
      std::shared_ptr<reader::ReadSharedState> state) {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "ODPS_SCAN: streaming ODPS tables is not yet implemented (module 1, "
        "task T106 pending)");
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
