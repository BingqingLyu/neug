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

#include <arrow/filesystem/filesystem.h>
#include <arrow/filesystem/localfs.h>
#include <memory>

#include "metadata/iceberg_metadata.h"
#include "metadata/iceberg_manifest.h"
#include "metadata/iceberg_snapshot.h"
#include "schema/iceberg_type_mapper.h"
#include "iceberg_options.h"

#include "neug/compiler/function/function.h"
#include "neug/compiler/function/read_function.h"
#include "neug/compiler/main/metadata_registry.h"
#include "neug/utils/reader/options.h"
#include "neug/utils/reader/reader.h"
#include "neug/utils/reader/schema.h"
#include "neug/utils/reader/sniffer.h"

namespace neug {
namespace function {

struct IcebergReadFunction {
  static constexpr const char* name = "ICEBERG_SCAN";

  static function_set getFunctionSet() {
    auto typeIDs =
        std::vector<common::LogicalTypeID>{common::LogicalTypeID::STRING};
    auto readFunction = std::make_unique<ReadFunction>(name, typeIDs);
    readFunction->execFunc = execFunc;
    readFunction->sniffFunc = sniffFunc;
    function_set functionSet;
    functionSet.push_back(std::move(readFunction));
    return functionSet;
  }

  static execution::Context execFunc(
      std::shared_ptr<reader::ReadSharedState> state);

  static std::shared_ptr<reader::EntrySchema> sniffFunc(
      const reader::FileSchema& schema);

  /**
   * @brief Probes whether a given path looks like an Iceberg table.
   *
   * Checks for the presence of metadata/v*.metadata.json files.
   * Used for auto-detection in the sniffer pipeline.
   */
  static bool probe(const std::string& path, fsys::FileSystem* fs);
};

}  // namespace function
}  // namespace neug
