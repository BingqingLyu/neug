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

#include "parquet_export_function.h"

#include <arrow/table.h>
#include <arrow/type.h>

#include "neug/compiler/main/metadata_registry.h"
#include "neug/utils/exception/exception.h"

namespace neug {
namespace writer {

// Convert NeuG LogicalType to Arrow DataType
static std::shared_ptr<arrow::DataType> logicalTypeToArrow(
    const common::LogicalType& type) {
  using common::LogicalTypeID;
  
  switch (type.getLogicalTypeID()) {
    case LogicalTypeID::INT64:
      return arrow::int64();
    case LogicalTypeID::INT32:
      return arrow::int32();
    case LogicalTypeID::UINT64:
      return arrow::uint64();
    case LogicalTypeID::UINT32:
      return arrow::uint32();
    case LogicalTypeID::DOUBLE:
      return arrow::float64();
    case LogicalTypeID::FLOAT:
      return arrow::float32();
    case LogicalTypeID::BOOLEAN:
      return arrow::boolean();
    case LogicalTypeID::STRING:
      return arrow::utf8();
    case LogicalTypeID::DATE:
      return arrow::date32();
    case LogicalTypeID::TIMESTAMP:
      return arrow::timestamp(arrow::TimeUnit::MICROSECOND);
    default:
      THROW_INVALID_ARGUMENT_EXCEPTION("Unsupported type for Parquet export: " +
                                       type.toString());
  }
}

// Convert QueryResponse to Arrow Table
static std::shared_ptr<arrow::Table> convertToArrowTable(
    const QueryResponse* table,
    const std::shared_ptr<arrow::Schema>& schema) {
  std::vector<std::shared_ptr<arrow::ChunkedArray>> columns;
  auto num_columns = table->columns_size();
  
  for (int i = 0; i < num_columns; ++i) {
    auto& ctx_col = table->columns(i);
    
    // Extract Arrow Array from context column
    // Note: This assumes ArrowArrayContextColumn - may need adjustment
    // based on actual column type
    if (ctx_col.has_arrow_array()) {
      columns.push_back(ctx_col.arrow_array());
    } else {
      // For non-Arrow columns, we need to convert them
      // This is a simplified implementation - full conversion needed
      THROW_INVALID_ARGUMENT_EXCEPTION(
          "Non-Arrow column type not supported yet for Parquet export");
    }
  }
  
  return arrow::Table::Make(schema, columns);
}

neug::Status ArrowParquetExportWriter::initializeWriter(
    const QueryResponse* first_table) {
  try {
    // 1. Build Arrow Schema from first batch
    std::vector<std::shared_ptr<arrow::Field>> fields;
    auto num_columns = first_table->columns_size();
    
    for (int i = 0; i < num_columns; ++i) {
      auto& ctx_col = first_table->columns(i);
      auto column_name = ctx_col.name();
      
      // Get type from entry_schema_ or infer from column
      // For now, we'll use a simplified approach
      // TODO: Properly extract type from entry_schema_
      auto arrow_type = arrow::utf8();  // Placeholder
      
      fields.push_back(arrow::field(column_name, arrow_type));
    }
    
    auto arrow_schema = arrow::schema(fields);
    
    // 2. Configure WriterProperties with defaults
    auto props = parquet::WriterProperties::Builder()
        .compression(arrow::Compression::SNAPPY)
        .max_row_group_length(1048576)  // 1M rows
        .enable_dictionary()
        .build();
    
    auto arrow_props = parquet::ArrowWriterProperties::Builder()
        .store_schema()  // Store Arrow schema for easier reads
        .build();
    
    // 3. Open output file
    if (schema_.paths.empty()) {
      return neug::Status(neug::StatusCode::ERR_INVALID_ARGUMENT,
                          "No output path specified for Parquet export");
    }
    
    ARROW_ASSIGN_OR_RAISE(outfile_,
                          fileSystem_->OpenOutputStream(schema_.paths[0]));
    
    // 4. Create FileWriter
    ARROW_ASSIGN_OR_RAISE(
        parquet_writer_,
        parquet::arrow::FileWriter::Open(*arrow_schema,
                                          arrow::default_memory_pool(),
                                          outfile_,
                                          props,
                                          arrow_props));
    
    initialized_ = true;
    return neug::Status::OK();
  } catch (const std::exception& e) {
    return neug::Status(neug::StatusCode::ERR_IO,
                        std::string("Failed to initialize Parquet writer: ") +
                            e.what());
  }
}

neug::Status ArrowParquetExportWriter::writeTable(
    const QueryResponse* table) {
  try {
    // 1. Initialize on first batch
    if (!initialized_) {
      auto status = initializeWriter(table);
      if (!status.ok()) {
        return status;
      }
    }
    
    // 2. Get schema from first initialization
    // Note: We need to store the schema during initialization
    // For now, this is a simplified implementation
    
    // 3. Convert batch to Arrow Table
    // TODO: Properly build schema from entry_schema_
    auto arrow_schema = parquet_writer_->schema();
    ARROW_ASSIGN_OR_RAISE(auto arrow_table,
                          convertToArrowTable(table, arrow_schema));
    
    // 4. Write Table (creates one row group per batch)
    ARROW_RETURN_NOT_OK(
        parquet_writer_->WriteTable(*arrow_table, arrow_table->num_rows()));
    
    return neug::Status::OK();
  } catch (const std::exception& e) {
    return neug::Status(neug::StatusCode::ERR_IO,
                        std::string("Failed to write Parquet table: ") +
                            e.what());
  }
}

ArrowParquetExportWriter::~ArrowParquetExportWriter() {
  if (parquet_writer_ && initialized_) {
    auto status = parquet_writer_->Close();
    if (!status.ok()) {
      LOG(ERROR) << "Failed to close Parquet writer: " << status.ToString();
    }
  }
}

}  // namespace writer

namespace function {

// Export function execution
static execution::Context parquetExecFunc(
    neug::execution::Context& ctx, reader::FileSchema& schema,
    const std::shared_ptr<reader::EntrySchema>& entry_schema,
    const neug::StorageReadInterface& graph) {
  if (schema.paths.empty()) {
    THROW_INVALID_ARGUMENT_EXCEPTION("Schema paths is empty");
  }
  
  const auto& vfs = neug::main::MetadataRegistry::getVFS();
  const auto& fs = vfs->Provide(schema);
  
  auto writer = std::make_shared<neug::writer::ArrowParquetExportWriter>(
      schema, fs->toArrowFileSystem(), entry_schema);
  
  auto status = writer->write(ctx, graph);
  if (!status.ok()) {
    THROW_IO_EXCEPTION("Parquet export failed: " + status.ToString());
  }
  
  ctx.clear();
  return ctx;
}

// Bind function (reuse pattern from JSON export)
static std::unique_ptr<ExportFuncBindData> bindFunc(
    ExportFuncBindInput& bindInput) {
  return std::make_unique<ExportFuncBindData>(
      bindInput.columnNames, bindInput.filePath, bindInput.parsingOptions);
}

function_set ExportParquetFunction::getFunctionSet() {
  function_set functionSet;
  auto exportFunc = std::make_unique<ExportFunction>(name);
  exportFunc->bind = bindFunc;
  exportFunc->execFunc = parquetExecFunc;
  functionSet.push_back(std::move(exportFunc));
  return functionSet;
}

}  // namespace function
}  // namespace neug
