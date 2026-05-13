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

#include "iceberg_read_function.h"

#include <arrow/dataset/file_parquet.h>

#include "glog/logging.h"
#include "iceberg_options.h"
#include "neug/utils/exception/exception.h"
#include "neug/utils/reader/sniffer.h"

namespace neug {
namespace function {

bool IcebergReadFunction::probe(const std::string& path,
                                fsys::FileSystem* fs) {
  try {
    std::string glob_pattern = path + "/metadata/v*.metadata.json";
    auto files = fs->glob(glob_pattern);
    return !files.empty();
  } catch (...) {
    return false;
  }
}

std::shared_ptr<reader::EntrySchema> IcebergReadFunction::sniffFunc(
    const reader::FileSchema& schema) {
  // Get filesystem
  const auto& vfs = neug::main::MetadataRegistry::getVFS();
  const auto& fs = vfs->Provide(schema);

  if (schema.paths.empty()) {
    THROW_IO_EXCEPTION("[iceberg] No path provided for Iceberg table scan.");
  }

  const std::string& table_path = schema.paths[0];

  // Parse scan options
  iceberg::IcebergScanOptions scan_options =
      iceberg::parseScanOptions(schema.options);

  // Resolve table: metadata → snapshot → manifests → data files
  auto resolved = iceberg::resolveTable(table_path, scan_options, fs.get());

  if (resolved.data_files.empty()) {
    THROW_IO_EXCEPTION(
        "[iceberg] Table at '" + table_path +
        "' has no data files. Cannot infer schema from empty table.");
  }

  // Collect data file paths
  std::vector<std::string> data_file_paths;
  data_file_paths.reserve(resolved.data_files.size());
  for (const auto& df : resolved.data_files) {
    data_file_paths.push_back(df.file_path);
  }

  // Set up ReadSharedState pointing at the Parquet data files
  auto state = std::make_shared<reader::ReadSharedState>();
  auto& externalSchema = state->schema;
  externalSchema.entry = std::make_shared<reader::TableEntrySchema>();
  externalSchema.file = schema;
  externalSchema.file.paths = std::move(data_file_paths);

  // Create Iceberg options builder (Parquet format)
  auto optionsBuilder =
      std::make_unique<reader::ArrowIcebergOptionsBuilder>(state);

  // Create ArrowReader and ArrowSniffer to infer schema from Parquet files
  auto reader = std::make_shared<reader::ArrowReader>(
      state, std::move(optionsBuilder), fs->toArrowFileSystem());
  auto sniffer = std::make_shared<reader::ArrowSniffer>(reader);
  auto sniffResult = sniffer->sniff();

  if (!sniffResult) {
    THROW_IO_EXCEPTION("[iceberg] Failed to sniff Parquet schema: " +
                       sniffResult.error().ToString());
  }
  return sniffResult.value();
}

execution::Context IcebergReadFunction::execFunc(
    std::shared_ptr<reader::ReadSharedState> state) {
  // Get filesystem
  const auto& vfs = neug::main::MetadataRegistry::getVFS();
  const auto& fs = vfs->Provide(state->schema.file);

  if (state->schema.file.paths.empty()) {
    THROW_IO_EXCEPTION("[iceberg] No path provided for Iceberg table scan.");
  }

  const std::string& table_path = state->schema.file.paths[0];

  // Parse scan options from LOAD FROM inline parameters
  iceberg::IcebergScanOptions scan_options =
      iceberg::parseScanOptions(state->schema.file.options);

  // Resolve table: metadata → snapshot → manifests → data files
  auto resolved = iceberg::resolveTable(table_path, scan_options, fs.get());

  if (resolved.data_files.empty()) {
    // Empty table — return empty context with correct schema
    LOG(INFO) << "[iceberg] Table at '" << table_path
              << "' has no data files in the selected snapshot.";
    execution::Context ctx;
    return ctx;
  }

  // Collect data file paths for reading via ArrowReader
  std::vector<std::string> data_file_paths;
  data_file_paths.reserve(resolved.data_files.size());
  for (const auto& df : resolved.data_files) {
    data_file_paths.push_back(df.file_path);
  }

  // Update state to point at the resolved Parquet data files
  state->schema.file.paths = std::move(data_file_paths);

  // Create Iceberg options builder (data files are Parquet)
  auto optionsBuilder =
      std::make_unique<reader::ArrowIcebergOptionsBuilder>(state);

  // Create ArrowReader to read the Parquet data files
  auto reader = std::make_unique<reader::ArrowReader>(
      state, std::move(optionsBuilder), fs->toArrowFileSystem());

  // Execute read
  execution::Context ctx;
  auto localState = std::make_shared<reader::ReadLocalState>();
  reader->read(localState, ctx);
  return ctx;
}

}  // namespace function
}  // namespace neug
