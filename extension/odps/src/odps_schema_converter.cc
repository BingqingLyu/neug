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

#include <unordered_map>
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
  // Module 4 (T401): temporal types map to NeuG kDate/kTimestampMs and DECIMAL
  // maps to DOUBLE (lossy; see convertScalarType).
  case OdpsTypeCode::kDecimal:
  case OdpsTypeCode::kDate:
  case OdpsTypeCode::kDatetime:
  case OdpsTypeCode::kTimestamp:
  case OdpsTypeCode::kTimestampNtz:
    return true;
  default:
    return false;
  }
}

namespace {

// Actionable, per-type explanation for a MaxCompute type the ODPS reader
// cannot materialize (module 4, task T404). Each message names the concrete
// NeuG limitation and, where one exists, a workaround the user can apply on
// the MaxCompute side.
std::string unsupportedReason(int typeCode) {
  switch (static_cast<OdpsTypeCode>(typeCode)) {
  case OdpsTypeCode::kBinary:
    return "NeuG's read path has no binary/blob type (DataTypeId::kBlob is "
           "unavailable), so raw bytes cannot be decoded losslessly; encode "
           "the column as STRING (e.g. BASE64) in MaxCompute to read it";
  case OdpsTypeCode::kIntervalYearMonth:
  case OdpsTypeCode::kIntervalDayTime:
    return "INTERVAL values are not decoded from the ODPS Arrow stream yet; "
           "cast the column to a numeric or STRING in MaxCompute to read it";
  case OdpsTypeCode::kArray:
  case OdpsTypeCode::kMap:
  case OdpsTypeCode::kStruct:
    return "complex/nested types are not supported by the ODPS reader, which "
           "decodes flat scalar columns only; flatten or extract the needed "
           "fields into scalar columns in MaxCompute";
  case OdpsTypeCode::kJson:
    return "the Storage API read path does not support JSON columns; cast the "
           "column to STRING in MaxCompute to read it";
  default:
    return "this MaxCompute type is not recognized by the ODPS reader";
  }
}

// NeuG's two internal->proto temporal emitters disagree on the oneof case for
// the same logical type (inferCommonType: kTimestampMs -> temporal.timestamp;
// gopt: kTimestampMs -> temporal.date_time), while the read path folds both
// date32/date -> kDate and timestamp/date_time -> kTimestampMs. So the schema
// guard must compare temporal columns by normalized class, not raw proto
// bytes, or a plain `RETURN *` false-positives when the optimizer-rewritten
// requested schema and the re-sniffed native schema used different emitters.
int temporalClass(const ::common::Temporal& temporal) {
  switch (temporal.item_case()) {
  case ::common::Temporal::kDate:
  case ::common::Temporal::kDate32:
    return 1;  // DATE (int32 days since epoch)
  case ::common::Temporal::kTimestamp:
  case ::common::Temporal::kDateTime:
    return 2;  // TIMESTAMP (int64 milliseconds since epoch)
  case ::common::Temporal::kTime:
  case ::common::Temporal::kTime32:
    return 3;  // TIME (int32 milliseconds past midnight)
  case ::common::Temporal::kInterval:
    return 4;  // INTERVAL
  default:
    return 0;  // unset / unknown
  }
}

}  // namespace

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
  // MaxCompute DECIMAL(p,s) has no exact NeuG read-path representation: the
  // proto->internal converter (NeuGTypeConverter::convert) has no decimal case
  // and DataTypeId has neither a decimal nor a kBlob, so a Decimal128 cannot
  // flow through a LOAD FROM scan. v1 maps DECIMAL to DOUBLE so such columns
  // stay readable; this is LOSSY -- values with more than ~15-17 significant
  // digits (ODPS DECIMAL defaults to (38,18)) lose precision. See
  // extension/odps/README.md ("Type mapping"). The arrow_bridge decodes the
  // Arrow `d:p,s` decimal128 column to a double to match this declared type.
  case OdpsTypeCode::kDecimal:
    type->set_primitive_type(::common::PrimitiveType::DT_DOUBLE);
    return type;
  // Temporal types: DATE -> NeuG kDate; DATETIME/TIMESTAMP/TIMESTAMP_NTZ ->
  // NeuG kTimestampMs (millisecond precision). Emit the SAME canonical proto
  // NeuG's inferCommonType produces (kDate -> temporal.date, kTimestampMs ->
  // temporal.timestamp) so the sniffed scan schema survives the planner's
  // proto->internal->proto round-trip byte-identically; a non-canonical
  // temporal.date32 here would make the M3 schema-mismatch guard
  // (findSchemaMismatch) false-positive on a plain `RETURN *`. The arrow_bridge
  // decodes the physical Arrow date32 (`tdD`) / timestamp (`ts{s,m,u,n}`)
  // layouts independently of this declared type; TIMESTAMP nanos/micros are
  // truncated to milliseconds, and the reader drops the Arrow timezone
  // annotation so a tz-annotated DATETIME/TIMESTAMP renders as its UTC instant
  // (see extension/odps/README.md, "Precision and timezone").
  case OdpsTypeCode::kDate:
    type->mutable_temporal()->mutable_date();
    return type;
  case OdpsTypeCode::kDatetime:
  case OdpsTypeCode::kTimestamp:
  case OdpsTypeCode::kTimestampNtz:
    type->mutable_temporal()->mutable_timestamp();
    return type;
  default:
    break;
  }
  THROW_INVALID_ARGUMENT_EXCEPTION(
      "ODPS_SCAN: column \"" + columnName + "\" has MaxCompute type " +
      typeName(typeCode) + ", which the ODPS reader cannot read: " +
      unsupportedReason(typeCode) +
      ". Supported types: integer widths (TINYINT/SMALLINT/INT/BIGINT), "
      "FLOAT, DOUBLE, BOOLEAN, STRING/VARCHAR/CHAR, DATE, "
      "DATETIME/TIMESTAMP, and DECIMAL (read as DOUBLE, lossy).");
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

bool OdpsSchemaConverter::sameScalarType(const ::common::DataType& requested,
                                         const ::common::DataType& native) {
  // STRING/VARCHAR/CHAR all collapse to the same NeuG string type, so any two
  // string types are equivalent regardless of the underlying item case.
  if (requested.has_string() && native.has_string()) {
    return true;
  }
  if (requested.item_case() != native.item_case()) {
    return false;
  }
  switch (requested.item_case()) {
  case ::common::DataType::kPrimitiveType:
    return requested.primitive_type() == native.primitive_type();
  case ::common::DataType::kTemporal:
    // Compare by normalized class: NeuG's inferCommonType and gopt emitters
    // pick different oneof cases (timestamp vs date_time) for the same logical
    // TIMESTAMP, so raw-byte equality would false-positive on `RETURN *`.
    return temporalClass(requested.temporal()) ==
           temporalClass(native.temporal());
  default:
    return requested.SerializeAsString() == native.SerializeAsString();
  }
}

std::string OdpsSchemaConverter::findSchemaMismatch(
    const reader::EntrySchema& requested, const reader::EntrySchema& native) {
  // Index the native schema by column name: the requested schema preserves the
  // sniffed column order but the optimizer may have rewritten individual column
  // types in place, so match by name rather than position.
  std::unordered_map<std::string, const ::common::DataType*> nativeByName;
  nativeByName.reserve(native.columnNames.size());
  for (size_t i = 0; i < native.columnNames.size(); ++i) {
    if (i < native.columnTypes.size() && native.columnTypes[i]) {
      nativeByName[native.columnNames[i]] = native.columnTypes[i].get();
    }
  }
  for (size_t i = 0; i < requested.columnNames.size(); ++i) {
    if (i >= requested.columnTypes.size() || !requested.columnTypes[i]) {
      continue;
    }
    auto it = nativeByName.find(requested.columnNames[i]);
    // A column absent from the native schema is not a type mismatch; leave it
    // to the reader/projection to resolve (or reject) by name.
    if (it == nativeByName.end()) {
      continue;
    }
    if (!sameScalarType(*requested.columnTypes[i], *it->second)) {
      return requested.columnNames[i];
    }
  }
  return "";
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
