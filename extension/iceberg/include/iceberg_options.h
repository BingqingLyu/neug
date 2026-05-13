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

#include "neug/utils/reader/options.h"
#include "neug/utils/reader/reader.h"

namespace neug {
namespace reader {

/**
 * @brief Arrow options builder for Iceberg data files (Parquet format).
 *
 * Iceberg stores data in Parquet files. This builder configures Arrow's
 * Parquet reader with Iceberg-appropriate defaults.
 */
class ArrowIcebergOptionsBuilder : public ArrowOptionsBuilder {
 public:
  explicit ArrowIcebergOptionsBuilder(std::shared_ptr<ReadSharedState> state)
      : ArrowOptionsBuilder(state){};

  ArrowOptions build() const override;

 protected:
  std::shared_ptr<arrow::dataset::FragmentScanOptions> buildFragmentOptions()
      const;

  std::shared_ptr<arrow::dataset::FileFormat> buildFileFormat(
      const arrow::dataset::ScanOptions& options) const;
};

}  // namespace reader
}  // namespace neug
