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

#include "odps_schema_converter.h"

#include <unordered_set>
#include <utility>

#include "neug/utils/exception/exception.h"
#include "odps_connection.h"
#include "odps_error.h"

#if defined(ODPS_SDK_ENABLE_ARROW)
#include "odps_sdk_glue.h"
#endif

namespace neug {
namespace extension {
namespace odps {

std::string OdpsSchemaConverter::typeName(int typeCode) {
  switch (static_cast<OdpsTypeCode>(typeCode)) {
  case OdpsTypeCode::kBigint:
    return "BIGINT";
  case OdpsTypeCode::kDouble:
    return "DOUBLE";
  case OdpsTypeCode::kBoolean:
    return "BOOLEAN";
  case OdpsTypeCode::kDatetime:
    return "DATETIME";
  case OdpsTypeCode::kString:
    return "STRING";
  case OdpsTypeCode::kDecimal:
    return "DECIMAL";
  case OdpsTypeCode::kTinyint:
    return "TINYINT";
  case OdpsTypeCode::kSmallint:
    return "SMALLINT";
  case OdpsTypeCode::kInteger:
    return "INT";
  case OdpsTypeCode::kChar:
    return "CHAR";
  case OdpsTypeCode::kVarchar:
    return "VARCHAR";
  case OdpsTypeCode::kBinary:
    return "BINARY";
  case OdpsTypeCode::kDate:
    return "DATE";
  case OdpsTypeCode::kTimestamp:
    return "TIMESTAMP";
  case OdpsTypeCode::kFloat:
    return "FLOAT";
  case OdpsTypeCode::kIntervalYearMonth:
    return "INTERVAL_YEAR_MONTH";
  case OdpsTypeCode::kIntervalDayTime:
    return "INTERVAL_DAY_TIME";
  case OdpsTypeCode::kArray:
    return "ARRAY";
  case OdpsTypeCode::kMap:
    return "MAP";
  case OdpsTypeCode::kStruct:
    return "STRUCT";
  case OdpsTypeCode::kJson:
    return "JSON";
  case OdpsTypeCode::kTimestampNtz:
    return "TIMESTAMP_NTZ";
  case OdpsTypeCode::kUnknown:
  default:
    return "UNKNOWN(" + std::to_string(typeCode) + ")";
  }
}

bool OdpsSchemaConverter::isSupportedScalar(int typeCode) {
  switch (static_cast<OdpsTypeCode>(typeCode)) {
  case OdpsTypeCode::kTinyint:
  case OdpsTypeCode::kSmallint:
  case OdpsTypeCode::kInteger:
  case OdpsTypeCode::kBigint:
  case OdpsTypeCode::kFloat:
  case OdpsTypeCode::kDouble:
  case OdpsTypeCode::kBoolean:
  case OdpsTypeCode::kString:
  case OdpsTypeCode::kVarchar:
  case OdpsTypeCode::kChar:
    return true;
  default:
    return false;
  }
}

std::shared_ptr<::common::DataType> OdpsSchemaConverter::convertScalarType(
    int typeCode, const std::string& columnName) {
  auto type = std::make_shared<::common::DataType>();
  switch (static_cast<OdpsTypeCode>(typeCode)) {
  // NeuG's DataType proto has no 8/16-bit integer primitive, so all signed
  // integer widths narrower than 64 bits collapse to SIGNED_INT32 (the same
  // convention the parquet extension applies to Arrow int8/int16/int32).
  case OdpsTypeCode::kTinyint:
  case OdpsTypeCode::kSmallint:
  case OdpsTypeCode::kInteger:
    type->set_primitive_type(::common::PrimitiveType::DT_SIGNED_INT32);
    return type;
  case OdpsTypeCode::kBigint:
    type->set_primitive_type(::common::PrimitiveType::DT_SIGNED_INT64);
    return type;
  case OdpsTypeCode::kFloat:
    type->set_primitive_type(::common::PrimitiveType::DT_FLOAT);
    return type;
  case OdpsTypeCode::kDouble:
    type->set_primitive_type(::common::PrimitiveType::DT_DOUBLE);
    return type;
  case OdpsTypeCode::kBoolean:
    type->set_primitive_type(::common::PrimitiveType::DT_BOOL);
    return type;
  case OdpsTypeCode::kString:
  case OdpsTypeCode::kVarchar:
  case OdpsTypeCode::kChar:
    type->mutable_string()->mutable_var_char();
    return type;
  default:
    break;
  }
  THROW_INVALID_ARGUMENT_EXCEPTION(
      "ODPS_SCAN: column \"" + columnName + "\" has MaxCompute type " +
      typeName(typeCode) +
      ", which is not supported in v1 (only integer/float/double/boolean/"
      "string types are; decimal, temporal, binary and complex types are "
      "planned for module 4, task T401)");
}

std::shared_ptr<reader::EntrySchema> OdpsSchemaConverter::convertColumns(
    const std::vector<OdpsColumnDesc>& columns) {
  if (columns.empty()) {
    THROW_INVALID_ARGUMENT_EXCEPTION("ODPS_SCAN: table has no columns to read");
  }
  auto entry = std::make_shared<reader::TableEntrySchema>();
  entry->columnNames.reserve(columns.size());
  entry->columnTypes.reserve(columns.size());
  std::unordered_set<std::string> seen;
  for (const auto& col : columns) {
    if (col.name.empty()) {
      THROW_INVALID_ARGUMENT_EXCEPTION(
          "ODPS_SCAN: unnamed column at index " +
          std::to_string(entry->columnNames.size()));
    }
    if (!seen.insert(col.name).second) {
      THROW_INVALID_ARGUMENT_EXCEPTION("ODPS_SCAN: duplicate column \"" +
                                       col.name + "\"");
    }
    entry->columnNames.push_back(col.name);
    entry->columnTypes.push_back(convertScalarType(col.typeCode, col.name));
  }
  return std::static_pointer_cast<reader::EntrySchema>(entry);
}

std::shared_ptr<reader::EntrySchema> OdpsSchemaConverter::sniffTableSchema(
    OdpsConnection& connection, const OdpsSourceDesc& source) {
#if defined(ODPS_SDK_ENABLE_ARROW)
  if (source.table.empty()) {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "ODPS_SCAN: cannot sniff schema without a table name");
  }
  // The address may omit the project; fall back to the connection default.
  const std::string project =
      source.hasProject() ? source.project : connection.options().project;
  if (project.empty()) {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "ODPS_SCAN: cannot sniff schema of \"" + source.table +
        "\": no project given in the odps:// address and no default project "
        "on the connection (set the `project` option or ODPS_PROJECT)");
  }

  auto* glue = static_cast<OdpsGlueConnection*>(connection.glueHandle());
  if (glue == nullptr) {
    THROW_RUNTIME_ERROR(
        "ODPS_SCAN: ODPS connection is unavailable on this handle");
  }

  // Read the authoritative column metadata through the ABI=0 glue seam. The
  // glue returns a POD array of C strings/ints that we copy into SDK-free
  // OdpsColumnDesc values, so no std::string ever crosses the ABI boundary.
  // Only the data columns are surfaced; partition columns act as filters in
  // the Storage API read path and are not emitted as row columns in v1.
  OdpsGlueSchema result;
  const int rc =
      odps_glue_sniff_schema(glue, project.c_str(), source.schema.c_str(),
                             source.table.c_str(), &result);
  if (rc != 0 || result.error != nullptr) {
    const std::string message =
        (result.error != nullptr)
            ? result.error
            : "unknown error (code " + std::to_string(rc) + ")";
    odps_glue_schema_free(&result);
    OdpsError::throwAttributed(
        "ODPS_SCAN: failed to read schema of " + project + "." + source.schema +
            "." + source.table,
        message,
        {connection.options().accessId, connection.options().accessKey});
  }

  std::vector<OdpsColumnDesc> columns;
  columns.reserve(result.count);
  for (size_t i = 0; i < result.count; ++i) {
    OdpsColumnDesc desc;
    desc.name =
        (result.columns[i].name != nullptr) ? result.columns[i].name : "";
    desc.typeCode = result.columns[i].type_code;
    desc.nullable = result.columns[i].nullable != 0;
    columns.push_back(std::move(desc));
  }
  odps_glue_schema_free(&result);
  return convertColumns(columns);
#else
  (void) connection;
  (void) source;
  THROW_INVALID_ARGUMENT_EXCEPTION(
      "ODPS_SCAN: live schema sniffing requires the ODPS SDK build (configure "
      "with -DNEUG_WITH_ODPS_SDK=ON). The pure type-mapping core is available, "
      "but table metadata cannot be fetched without the SDK.");
#endif
}

}  // namespace odps
}  // namespace extension
}  // namespace neug
