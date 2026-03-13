/**
 * Copyright 2020 Alibaba Group Holding Limited.
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

#include <arrow/dataset/dataset.h>
#include <arrow/dataset/file_base.h>
#include <arrow/dataset/file_parquet.h>
#include <memory>
#include "neug/utils/reader/options.h"
#include "neug/utils/reader/reader.h"

namespace neug {
namespace reader {

/**
 * @brief Parquet-specific parse options
 *
 * These options control Parquet file reading behavior:
 * - use_embedded_schema: Use schema from Parquet metadata (default: true)
 * - buffered_stream: Enable buffered I/O stream for better performance (default: true)
 * - pre_buffer: Pre-buffer data for high-latency filesystems like S3 (default: false)
 * - cache_decompressed: Cache decompressed data for repeated reads (default: true)
 * - row_batch_size: Number of rows per Arrow batch when converting from Parquet (default: 65536)
 *
 */
struct ParquetParseOptions {
  Option<bool> use_embedded_schema =
      Option<bool>::BoolOption("USE_EMBEDDED_SCHEMA", true);
  Option<bool> buffered_stream =
      Option<bool>::BoolOption("BUFFERED_STREAM", true);
  Option<bool> pre_buffer =
      Option<bool>::BoolOption("PRE_BUFFER", false);
  Option<bool> cache_decompressed =
      Option<bool>::BoolOption("CACHE_DECOMPRESSED", true);
  Option<int64_t> row_batch_size =
      Option<int64_t>::Int64Option("PARQUET_BATCH_ROWS", 65536);
};

/**
 * @brief Parquet-specific implementation of Arrow scan options builder
 *
 * This class extends ArrowOptionsBuilder to provide Parquet-specific
 * functionality:
 * - buildFragmentOptions(): Builds ParquetFragmentScanOptions with options
 *   for parallel reading, dictionary encoding, etc.
 * - buildFileFormat(): Builds ParquetFileFormat
 */
class ArrowParquetOptionsBuilder : public ArrowOptionsBuilder {
 public:
  /**
   * @brief Constructs an ArrowParquetOptionsBuilder with the given shared state
   * @param state The shared read state containing Parquet schema and configuration
   */
  explicit ArrowParquetOptionsBuilder(std::shared_ptr<ReadSharedState> state)
      : ArrowOptionsBuilder(state){};

  virtual ArrowOptions build() const override;

 protected:
  /**
   * @brief Builds Parquet-specific fragment scan options
   *
   * Creates ParquetFragmentScanOptions with:
   * - Reader properties: parallel reading, dictionary encoding, etc.
   *
   * @return ParquetFragmentScanOptions instance
   */
  std::shared_ptr<arrow::dataset::FragmentScanOptions> buildFragmentOptions()
      const;

  /**
   * @brief Builds ParquetFileFormat from scan options
   *
   * Extracts reader properties from the ParquetFragmentScanOptions
   * and configures the ParquetFileFormat.
   *
   * @param options The scan options containing fragment_scan_options
   * @return ParquetFileFormat instance configured with reader properties
   */
  std::shared_ptr<arrow::dataset::FileFormat> buildFileFormat(
      const arrow::dataset::ScanOptions& options) const;
};

}  // namespace reader
}  // namespace neug
