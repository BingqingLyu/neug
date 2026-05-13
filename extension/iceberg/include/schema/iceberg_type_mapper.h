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

#include <string>
#include <utility>
#include <vector>

#include "neug/compiler/common/types/types.h"

namespace neug {
namespace iceberg {

/**
 * @brief Column descriptor holding name, mapped NeuG type, and serialization
 * hints.
 */
struct IcebergColumnDescriptor {
  std::string name;
  common::LogicalTypeID type_id;
  bool needs_json_serialization = false;  // true for struct/list/map
};

/**
 * @brief Maps a single Iceberg type string to a NeuG LogicalTypeID.
 *
 * Supported types:
 *   boolean, int, long, float, double, decimal(p,s), date, time,
 *   timestamp, timestamptz, string, uuid, binary, fixed(L),
 *   struct<...>, list<...>, map<...>
 *
 * @param iceberg_type The Iceberg type string from metadata JSON.
 * @return The corresponding NeuG LogicalTypeID.
 * @throws Exception if the type is unrecognized.
 */
common::LogicalTypeID mapIcebergType(const std::string& iceberg_type);

/**
 * @brief Returns whether the given Iceberg type requires JSON serialization.
 *
 * Nested types (struct, list, map) are stored as JSON strings in NeuG.
 */
bool needsJsonSerialization(const std::string& iceberg_type);

/**
 * @brief Maps an Iceberg schema fields array (from metadata JSON) to a vector
 *        of column descriptors.
 *
 * @param fields JSON array of Iceberg schema fields, each with "name" and
 *        "type" keys.
 * @return Vector of (name, type_id, needs_json) descriptors.
 */
std::vector<IcebergColumnDescriptor> mapIcebergSchema(
    const std::vector<std::pair<std::string, std::string>>& fields);

}  // namespace iceberg
}  // namespace neug
