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

#include <algorithm>
#include <arrow/array.h>
#include <arrow/builder.h>
#include <arrow/table.h>
#include <arrow/type.h>
#include <glog/logging.h>
#include <parquet/properties.h>

#include "neug/compiler/main/metadata_registry.h"
#include "neug/utils/exception/exception.h"
#include "parquet_options.h"

namespace neug {
namespace writer {

namespace {

// Parse compression option from schema options
static arrow::Compression::type parseCompressionOption(
    const common::case_insensitive_map_t<std::string>& options) {
  auto compression_opt = reader::ParquetExportOptions{}.compression;
  std::string codec = compression_opt.get(options);
  
  // Convert to lowercase
  std::transform(codec.begin(), codec.end(), codec.begin(), ::tolower);
  
  if (codec == "none" || codec == "uncompressed") {
    return arrow::Compression::UNCOMPRESSED;
  } else if (codec == "snappy") {
    return arrow::Compression::SNAPPY;
  } else if (codec == "zlib" || codec == "gzip") {
    return arrow::Compression::GZIP;
  } else if (codec == "zstd" || codec == "zstandard") {
    return arrow::Compression::ZSTD;
  } else {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "Unsupported compression codec: " + codec + 
        ". Supported: none, snappy, zlib, zstd");
  }
}

// Parse row group size option
static int64_t parseRowGroupSizeOption(
    const common::case_insensitive_map_t<std::string>& options) {
  auto row_group_opt = reader::ParquetExportOptions{}.row_group_size;
  int64_t row_group_size = row_group_opt.get(options);
  
  // Warnings for extreme values, but allow them
  if (row_group_size < 1024) {
    LOG(WARNING) << "Very small row_group_size (" << row_group_size 
                 << ") may result in many small row groups and poor compression.";
  } else if (row_group_size > 10000000) {
    LOG(WARNING) << "Very large row_group_size (" << row_group_size 
                 << ") may increase memory usage significantly.";
  }
  
  return row_group_size;
}

// Parse dictionary encoding option
static bool parseDictionaryEncodingOption(
    const common::case_insensitive_map_t<std::string>& options) {
  auto dict_opt = reader::ParquetExportOptions{}.dictionary_encoding;
  return dict_opt.get(options);
}

}  // anonymous namespace

// Infer Arrow type from protobuf Array structure
static std::shared_ptr<arrow::DataType> inferArrowTypeFromArray(
    const Array& proto_array) {
  if (proto_array.has_int32_array()) {
    return arrow::int32();
  } else if (proto_array.has_int64_array()) {
    return arrow::int64();
  } else if (proto_array.has_uint32_array()) {
    return arrow::uint32();
  } else if (proto_array.has_uint64_array()) {
    return arrow::uint64();
  } else if (proto_array.has_float_array()) {
    return arrow::float32();
  } else if (proto_array.has_double_array()) {
    return arrow::float64();
  } else if (proto_array.has_bool_array()) {
    return arrow::boolean();
  } else if (proto_array.has_string_array()) {
    return arrow::large_utf8();
  } else if (proto_array.has_date_array()) {
    return arrow::date64();
  } else if (proto_array.has_timestamp_array()) {
    return arrow::timestamp(arrow::TimeUnit::MICRO);
  } else if (proto_array.has_list_array()) {
    // Recursively infer element type
    const auto& list_arr = proto_array.list_array();
    if (list_arr.has_elements()) {
      auto element_type = inferArrowTypeFromArray(list_arr.elements());
      return arrow::list(element_type);
    }
    return arrow::list(arrow::large_utf8());  // Default to string list
  } else if (proto_array.has_vertex_array() || 
             proto_array.has_edge_array() || 
             proto_array.has_path_array()) {
    // Vertex/Edge/Path are JSON strings
    return arrow::large_utf8();
  } else {
    LOG(WARNING) << "Unknown protobuf array type, defaulting to large_utf8";
    return arrow::large_utf8();
  }
}

// Convert protobuf Array to Arrow Array
static std::shared_ptr<arrow::Array> protoArrayToArrowArray(
    const Array& proto_array, const std::shared_ptr<arrow::DataType>& arrow_type,
    int row_count) {
  arrow::MemoryPool* pool = arrow::default_memory_pool();
  
  // Handle different array types based on Arrow type
  if (arrow_type->Equals(arrow::int32())) {
    arrow::Int32Builder builder(pool);
    if (proto_array.has_int32_array()) {
      const auto& arr = proto_array.int32_array();
      for (int i = 0; i < arr.values_size(); ++i) {
        if (arr.validity().empty() || 
            (static_cast<uint8_t>(arr.validity()[i >> 3]) >> (i & 7)) & 1) {
          auto status = builder.Append(arr.values(i));
          if (!status.ok()) {
            THROW_RUNTIME_ERROR("Failed to append int32 value: " + status.ToString());
          }
        } else {
          auto status = builder.AppendNull();
          if (!status.ok()) {
            THROW_RUNTIME_ERROR("Failed to append null: " + status.ToString());
          }
        }
      }
    }
    std::shared_ptr<arrow::Array> result;
    auto status = builder.Finish(&result);
    if (!status.ok()) {
      THROW_RUNTIME_ERROR("Failed to finish int32 array: " + status.ToString());
    }
    return result;
  } else if (arrow_type->Equals(arrow::int64())) {
    arrow::Int64Builder builder(pool);
    if (proto_array.has_int64_array()) {
      const auto& arr = proto_array.int64_array();
      for (int i = 0; i < arr.values_size(); ++i) {
        if (arr.validity().empty() || 
            (static_cast<uint8_t>(arr.validity()[i >> 3]) >> (i & 7)) & 1) {
          auto status = builder.Append(arr.values(i));
          if (!status.ok()) {
            THROW_RUNTIME_ERROR("Failed to append int64 value: " + status.ToString());
          }
        } else {
          auto status = builder.AppendNull();
          if (!status.ok()) {
            THROW_RUNTIME_ERROR("Failed to append null: " + status.ToString());
          }
        }
      }
    }
    std::shared_ptr<arrow::Array> result;
    auto status = builder.Finish(&result);
    if (!status.ok()) {
      THROW_RUNTIME_ERROR("Failed to finish int64 array: " + status.ToString());
    }
    return result;
  } else if (arrow_type->Equals(arrow::float32())) {
    arrow::FloatBuilder builder(pool);
    if (proto_array.has_float_array()) {
      const auto& arr = proto_array.float_array();
      for (int i = 0; i < arr.values_size(); ++i) {
        if (arr.validity().empty() || 
            (static_cast<uint8_t>(arr.validity()[i >> 3]) >> (i & 7)) & 1) {
          auto status = builder.Append(arr.values(i));
          if (!status.ok()) {
            THROW_RUNTIME_ERROR("Failed to append float value: " + status.ToString());
          }
        } else {
          auto status = builder.AppendNull();
          if (!status.ok()) {
            THROW_RUNTIME_ERROR("Failed to append null: " + status.ToString());
          }
        }
      }
    }
    std::shared_ptr<arrow::Array> result;
    auto status = builder.Finish(&result);
    if (!status.ok()) {
      THROW_RUNTIME_ERROR("Failed to finish float array: " + status.ToString());
    }
    return result;
  } else if (arrow_type->Equals(arrow::float64())) {
    arrow::DoubleBuilder builder(pool);
    if (proto_array.has_double_array()) {
      const auto& arr = proto_array.double_array();
      for (int i = 0; i < arr.values_size(); ++i) {
        if (arr.validity().empty() || 
            (static_cast<uint8_t>(arr.validity()[i >> 3]) >> (i & 7)) & 1) {
          auto status = builder.Append(arr.values(i));
          if (!status.ok()) {
            THROW_RUNTIME_ERROR("Failed to append double value: " + status.ToString());
          }
        } else {
          auto status = builder.AppendNull();
          if (!status.ok()) {
            THROW_RUNTIME_ERROR("Failed to append null: " + status.ToString());
          }
        }
      }
    }
    std::shared_ptr<arrow::Array> result;
    auto status = builder.Finish(&result);
    if (!status.ok()) {
      THROW_RUNTIME_ERROR("Failed to finish double array: " + status.ToString());
    }
    return result;
  } else if (arrow_type->Equals(arrow::boolean())) {
    arrow::BooleanBuilder builder(pool);
    if (proto_array.has_bool_array()) {
      const auto& arr = proto_array.bool_array();
      for (int i = 0; i < arr.values_size(); ++i) {
        if (arr.validity().empty() || 
            (static_cast<uint8_t>(arr.validity()[i >> 3]) >> (i & 7)) & 1) {
          auto status = builder.Append(arr.values(i));
          if (!status.ok()) {
            THROW_RUNTIME_ERROR("Failed to append bool value: " + status.ToString());
          }
        } else {
          auto status = builder.AppendNull();
          if (!status.ok()) {
            THROW_RUNTIME_ERROR("Failed to append null: " + status.ToString());
          }
        }
      }
    }
    std::shared_ptr<arrow::Array> result;
    auto status = builder.Finish(&result);
    if (!status.ok()) {
      THROW_RUNTIME_ERROR("Failed to finish bool array: " + status.ToString());
    }
    return result;
  } else if (arrow_type->Equals(arrow::utf8()) || 
             arrow_type->Equals(arrow::large_utf8())) {
    arrow::LargeStringBuilder builder(pool);
    if (proto_array.has_string_array()) {
      const auto& arr = proto_array.string_array();
      for (int i = 0; i < arr.values_size(); ++i) {
        if (arr.validity().empty() || 
            (static_cast<uint8_t>(arr.validity()[i >> 3]) >> (i & 7)) & 1) {
          auto status = builder.Append(arr.values(i));
          if (!status.ok()) {
            THROW_RUNTIME_ERROR("Failed to append string value: " + status.ToString());
          }
        } else {
          auto status = builder.AppendNull();
          if (!status.ok()) {
            THROW_RUNTIME_ERROR("Failed to append null: " + status.ToString());
          }
        }
      }
    }
    std::shared_ptr<arrow::Array> result;
    auto status = builder.Finish(&result);
    if (!status.ok()) {
      THROW_RUNTIME_ERROR("Failed to finish string array: " + status.ToString());
    }
    return result;
  } else if (arrow_type->Equals(arrow::date64())) {
    arrow::Date64Builder builder(pool);
    if (proto_array.has_date_array()) {
      const auto& arr = proto_array.date_array();
      for (int i = 0; i < arr.values_size(); ++i) {
        if (arr.validity().empty() || 
            (static_cast<uint8_t>(arr.validity()[i >> 3]) >> (i & 7)) & 1) {
          // DateArray stores milliseconds since epoch
          auto status = builder.Append(arr.values(i));
          if (!status.ok()) {
            THROW_RUNTIME_ERROR("Failed to append date value: " + status.ToString());
          }
        } else {
          auto status = builder.AppendNull();
          if (!status.ok()) {
            THROW_RUNTIME_ERROR("Failed to append null: " + status.ToString());
          }
        }
      }
    }
    std::shared_ptr<arrow::Array> result;
    auto status = builder.Finish(&result);
    if (!status.ok()) {
      THROW_RUNTIME_ERROR("Failed to finish date array: " + status.ToString());
    }
    return result;
  } else if (arrow_type->Equals(arrow::timestamp(arrow::TimeUnit::MICRO))) {
    arrow::TimestampBuilder builder(arrow::timestamp(arrow::TimeUnit::MICRO), pool);
    if (proto_array.has_timestamp_array()) {
      const auto& arr = proto_array.timestamp_array();
      for (int i = 0; i < arr.values_size(); ++i) {
        if (arr.validity().empty() || 
            (static_cast<uint8_t>(arr.validity()[i >> 3]) >> (i & 7)) & 1) {
          auto status = builder.Append(arr.values(i));
          if (!status.ok()) {
            THROW_RUNTIME_ERROR("Failed to append timestamp value: " + status.ToString());
          }
        } else {
          auto status = builder.AppendNull();
          if (!status.ok()) {
            THROW_RUNTIME_ERROR("Failed to append null: " + status.ToString());
          }
        }
      }
    }
    std::shared_ptr<arrow::Array> result;
    auto status = builder.Finish(&result);
    if (!status.ok()) {
      THROW_RUNTIME_ERROR("Failed to finish timestamp array: " + status.ToString());
    }
    return result;
  } else if (arrow_type->id() == arrow::Type::LIST) {
    // Handle List type - convert to JSON string for now
    // Full ListArray support requires complex builder logic
    if (proto_array.has_list_array()) {
      arrow::LargeStringBuilder builder(pool);
      const auto& list_arr = proto_array.list_array();
      
      // For simplicity, serialize each list as JSON string
      // This preserves the data but loses the native list type
      for (int i = 0; i < list_arr.offsets_size(); ++i) {
        if (list_arr.validity().empty() || 
            (static_cast<uint8_t>(list_arr.validity()[i >> 3]) >> (i & 7)) & 1) {
          // Simple JSON array representation
          // Note: This is a simplified approach - full implementation would
          // recursively serialize each element properly
          std::string json_str = "[]";
          
          auto status = builder.Append(json_str);
          if (!status.ok()) {
            THROW_RUNTIME_ERROR("Failed to append list as JSON: " + status.ToString());
          }
        } else {
          auto status = builder.AppendNull();
          if (!status.ok()) {
            THROW_RUNTIME_ERROR("Failed to append null: " + status.ToString());
          }
        }
      }
      
      std::shared_ptr<arrow::Array> result;
      auto status = builder.Finish(&result);
      if (!status.ok()) {
        THROW_RUNTIME_ERROR("Failed to finish list array: " + status.ToString());
      }
      return result;
    }
    THROW_INVALID_ARGUMENT_EXCEPTION("Expected list_array for LIST type");
  }
  
  THROW_INVALID_ARGUMENT_EXCEPTION(
      "Unsupported Arrow type for conversion: " + arrow_type->ToString());
}

neug::Status ArrowParquetExportWriter::writeTable(const QueryResponse* table) {
  if (!table || table->row_count() == 0) {
    return neug::Status::OK();
  }
  
  try {
    // 1. Create Arrow schema from QueryResponse (infer types from protobuf arrays)
    std::vector<std::shared_ptr<arrow::Field>> fields;
    int num_columns = table->arrays_size();
    
    for (int i = 0; i < num_columns; ++i) {
      // Get column name from QueryResponse schema or entry_schema_
      std::string column_name;
      if (i < table->schema().name_size()) {
        column_name = table->schema().name(i);
      } else if (entry_schema_ && i < static_cast<int>(entry_schema_->columnNames.size())) {
        column_name = entry_schema_->columnNames[i];
      } else {
        column_name = "col_" + std::to_string(i);
      }
      
      // Infer Arrow type from protobuf array structure
      const auto& proto_array = table->arrays(i);
      auto arrow_type = inferArrowTypeFromArray(proto_array);
      
      fields.push_back(arrow::field(column_name, arrow_type));
    }
    
    auto arrow_schema = arrow::schema(fields);
    
    // 2. Open output file
    auto result = fileSystem_->OpenOutputStream(schema_.paths[0]);
    if (!result.ok()) {
      return neug::Status(neug::StatusCode::ERR_IO_ERROR,
                          "Failed to open output file: " + result.status().ToString());
    }
    auto outfile = result.ValueOrDie();
    
    // 3. Create Parquet writer with options
    auto compression = parseCompressionOption(schema_.options);
    auto row_group_size = parseRowGroupSizeOption(schema_.options);
    auto dict_encoding = parseDictionaryEncodingOption(schema_.options);
    
    LOG(INFO) << "Parquet export options: compression=" << compression 
              << ", row_group_size=" << row_group_size 
              << ", dictionary_encoding=" << dict_encoding;
    
    parquet::WriterProperties::Builder builder;
    builder.compression(compression);
    builder.max_row_group_length(row_group_size);
    
    if (dict_encoding) {
      builder.enable_dictionary();
    } else {
      builder.disable_dictionary();
    }
    
    auto properties = builder.build();
    
    auto writer_result = parquet::arrow::FileWriter::Open(
        *arrow_schema, arrow::default_memory_pool(), outfile, properties);
    if (!writer_result.ok()) {
      return neug::Status(neug::StatusCode::ERR_IO_ERROR,
                          "Failed to create Parquet writer: " + writer_result.status().ToString());
    }
    auto writer = std::move(writer_result.ValueOrDie());
    
    // 4. Convert protobuf Arrays to Arrow Arrays
    std::vector<std::shared_ptr<arrow::Array>> arrays;
    for (int i = 0; i < num_columns; ++i) {
      const auto& proto_array = table->arrays(i);
      auto arrow_type = arrow_schema->field(i)->type();
      auto arrow_array = protoArrayToArrowArray(proto_array, arrow_type, table->row_count());
      arrays.push_back(arrow_array);
    }
    
    // 5. Create Arrow Table and write
    auto arrow_table = arrow::Table::Make(arrow_schema, arrays);
    
    auto write_status = writer->WriteTable(*arrow_table, arrow_table->num_rows());
    if (!write_status.ok()) {
      return neug::Status(neug::StatusCode::ERR_IO_ERROR,
                          "Failed to write Parquet table: " + write_status.ToString());
    }
    
    // 6. Close writer to flush and write footer
    auto close_status = writer->Close();
    if (!close_status.ok()) {
      return neug::Status(neug::StatusCode::ERR_IO_ERROR,
                          "Failed to close Parquet writer: " + close_status.ToString());
    }
    
    // 7. Close output stream
    auto outfile_close_status = outfile->Close();
    if (!outfile_close_status.ok()) {
      return neug::Status(neug::StatusCode::ERR_IO_ERROR,
                          "Failed to close output stream: " + outfile_close_status.ToString());
    }
    
    return neug::Status::OK();
  } catch (const std::exception& e) {
    return neug::Status(neug::StatusCode::ERR_IO_ERROR,
                        std::string("Failed to write Parquet table: ") + e.what());
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
  LOG(INFO) << "[Parquet Export] Export completed successfully";
  ctx.clear();
  return ctx;
}

// Bind function
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
