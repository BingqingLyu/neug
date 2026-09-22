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

#include "neug/generated/proto/plan/basic_type.pb.h"
#include "neug/utils/io/read/common/schema.h"
#include "odps_options.h"

namespace neug {
namespace extension {
namespace odps {

class OdpsConnection;

/**
 * @brief MaxCompute column type codes, mirroring the SDK's `ODPSColumnType`.
 *
 * The numeric values are identical to `apsara::odps::sdk::ODPSColumnType` so
 * the SDK-gated reader can `static_cast` an SDK enum straight into this one.
 * Keeping a SDK-free mirror means the mapping core (and its unit tests) compile
 * and run without `NEUG_WITH_ODPS_SDK` / Arrow.
 */
enum class OdpsTypeCode : int {
  kUnknown = -1,
  kBigint = 0,
  kDouble = 1,
  kBoolean = 2,
  kDatetime = 3,
  kString = 4,
  kDecimal = 5,
  kTinyint = 6,
  kSmallint = 7,
  kInteger = 8,
  kChar = 9,
  kVarchar = 10,
  kBinary = 11,
  kDate = 12,
  kTimestamp = 13,
  kFloat = 14,
  kIntervalYearMonth = 15,
  kIntervalDayTime = 16,
  kArray = 17,
  kMap = 18,
  kStruct = 19,
  kJson = 20,
  kTimestampNtz = 21,
};

/**
 * @brief SDK-free description of one ODPS column, as extracted by sniff.
 *
 * The SDK-gated reader fills this from `IODPSTableColumn`
 * (`GetName()` / `GetType()` / `GetNullable()`); the pure mapping core below
 * consumes it without any SDK dependency.
 */
struct OdpsColumnDesc {
  std::string name;
  // An `OdpsTypeCode` value; kept as a plain int so the descriptor stays a
  // trivial, SDK-free POD that tests can build with literal codes.
  int typeCode = static_cast<int>(OdpsTypeCode::kUnknown);
  bool nullable = false;
};

/**
 * @brief Converts MaxCompute table schemas into NeuG `reader::EntrySchema`.
 *
 * Split into two layers:
 *  - a pure, SDK-free mapping core (`convertScalarType` / `convertColumns`)
 *    that is fully unit-testable without the ODPS SDK; and
 *  - an SDK-gated `sniffTableSchema` that pulls the live `IODPSTableSchema`
 *    through the ODPS core API and feeds it into the pure core.
 *
 * v1 supports the basic scalar types only (integer widths, float, double,
 * boolean, string/varchar/char). Decimal, temporal, binary and complex types
 * (array/map/struct/json) are deferred to module 4 (T401) and raise a clear
 * "not yet supported" error rather than silently coercing.
 */
class OdpsSchemaConverter {
 public:
  // Human-readable MaxCompute type name for error messages (e.g. "DECIMAL").
  static std::string typeName(int typeCode);

  // Whether a type code is handled by the v1 scalar mapping.
  static bool isSupportedScalar(int typeCode);

  /**
   * @brief Map a single MaxCompute type code to a NeuG `::common::DataType`.
   * @throws exception::InvalidArgumentException for unsupported/unknown codes.
   */
  static std::shared_ptr<::common::DataType> convertScalarType(
      int typeCode, const std::string& columnName);

  /**
   * @brief Build a `TableEntrySchema` from an ordered list of columns.
   *
   * Column names are preserved as-is (MaxCompute names are already valid NeuG
   * identifiers); empty names and duplicates are rejected. Any column whose
   * type is unsupported in v1 raises the same located error as
   * `convertScalarType`.
   * @throws exception::InvalidArgumentException on empty/invalid column sets.
   */
  static std::shared_ptr<reader::EntrySchema> convertColumns(
      const std::vector<OdpsColumnDesc>& columns);

  /**
   * @brief Fetch the live table schema and convert it (SDK-gated).
   *
   * Uses the ODPS core API (`IODPS::Create(conf, project)` ->
   * `GetTables()->Get(project, schema, table)->GetSchema()`), which returns the
   * authoritative `IODPSTableSchema` even for empty tables (the Storage API
   * read path only exposes schema via a materialized Arrow batch). When the
   * extension is built without `NEUG_WITH_ODPS_SDK`, this throws a clear error
   * telling the user the SDK build is required for live sniffing.
   */
  static std::shared_ptr<reader::EntrySchema> sniffTableSchema(
      OdpsConnection& connection, const OdpsSourceDesc& source);
};

}  // namespace odps
}  // namespace extension
}  // namespace neug
