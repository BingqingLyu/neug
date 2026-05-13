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

#include "schema/iceberg_type_mapper.h"

#include <algorithm>
#include <cctype>

#include "neug/utils/exception/exception.h"

namespace neug {
namespace iceberg {

namespace {

// Trim whitespace from both ends
std::string trim(const std::string& s) {
  auto start = s.find_first_not_of(" \t\n\r");
  if (start == std::string::npos) return "";
  auto end = s.find_last_not_of(" \t\n\r");
  return s.substr(start, end - start + 1);
}

// Convert to lowercase for case-insensitive matching
std::string toLower(const std::string& s) {
  std::string result = s;
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return result;
}

// Check if type string starts with a given prefix (case-insensitive)
bool startsWith(const std::string& s, const std::string& prefix) {
  if (s.size() < prefix.size()) return false;
  return toLower(s.substr(0, prefix.size())) == prefix;
}

}  // namespace

common::LogicalTypeID mapIcebergType(const std::string& iceberg_type) {
  std::string type = toLower(trim(iceberg_type));

  // Primitive types
  if (type == "boolean") return common::LogicalTypeID::BOOL;
  if (type == "int" || type == "integer")
    return common::LogicalTypeID::INT32;
  if (type == "long") return common::LogicalTypeID::INT64;
  if (type == "float") return common::LogicalTypeID::FLOAT;
  if (type == "double") return common::LogicalTypeID::DOUBLE;
  if (type == "date") return common::LogicalTypeID::DATE;
  if (type == "timestamp") return common::LogicalTypeID::TIMESTAMP;
  if (type == "timestamptz") return common::LogicalTypeID::TIMESTAMP_TZ;
  if (type == "time") return common::LogicalTypeID::STRING;  // No native TIME
  if (type == "string") return common::LogicalTypeID::STRING;
  if (type == "uuid") return common::LogicalTypeID::UUID;
  if (type == "binary") return common::LogicalTypeID::BLOB;

  // Parameterized types
  if (startsWith(type, "decimal")) return common::LogicalTypeID::DECIMAL;
  if (startsWith(type, "fixed")) return common::LogicalTypeID::BLOB;

  // Nested types → JSON string serialization
  if (startsWith(type, "struct") || startsWith(type, "list") ||
      startsWith(type, "map")) {
    return common::LogicalTypeID::STRING;
  }

  THROW_EXCEPTION_WITH_FILE_LINE(
      "[iceberg] Unrecognized Iceberg type: '" + iceberg_type + "'");
}

bool needsJsonSerialization(const std::string& iceberg_type) {
  std::string type = toLower(trim(iceberg_type));
  return startsWith(type, "struct") || startsWith(type, "list") ||
         startsWith(type, "map");
}

std::vector<IcebergColumnDescriptor> mapIcebergSchema(
    const std::vector<std::pair<std::string, std::string>>& fields) {
  std::vector<IcebergColumnDescriptor> result;
  result.reserve(fields.size());
  for (const auto& [name, type] : fields) {
    IcebergColumnDescriptor desc;
    desc.name = name;
    desc.type_id = mapIcebergType(type);
    desc.needs_json_serialization = needsJsonSerialization(type);
    result.push_back(std::move(desc));
  }
  return result;
}

}  // namespace iceberg
}  // namespace neug
