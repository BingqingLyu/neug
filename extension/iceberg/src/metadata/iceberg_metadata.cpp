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

#include "metadata/iceberg_metadata.h"

#include <algorithm>
#include <regex>

#include <arrow/io/file.h>
#include <arrow/buffer.h>
#include <arrow/status.h>
#include "rapidjson/document.h"
#include "rapidjson/error/en.h"

#include "neug/utils/exception/exception.h"

namespace neug {
namespace iceberg {

namespace {

// Read entire file content from Arrow filesystem
std::string readFileContent(const std::string& path,
                            arrow::fs::FileSystem* arrow_fs) {
  auto result = arrow_fs->OpenInputFile(path);
  if (!result.ok()) {
    THROW_IO_EXCEPTION("[iceberg] Failed to open file '" + path +
                       "': " + result.status().ToString());
  }
  auto file = result.ValueOrDie();
  auto size_result = file->GetSize();
  if (!size_result.ok()) {
    THROW_IO_EXCEPTION("[iceberg] Failed to get size of '" + path +
                       "': " + size_result.status().ToString());
  }
  int64_t size = size_result.ValueOrDie();
  auto buf_result = file->Read(size);
  if (!buf_result.ok()) {
    THROW_IO_EXCEPTION("[iceberg] Failed to read file '" + path +
                       "': " + buf_result.status().ToString());
  }
  auto buffer = buf_result.ValueOrDie();
  return std::string(reinterpret_cast<const char*>(buffer->data()),
                     buffer->size());
}

// Extract version number from metadata filename like "v3.metadata.json"
int extractVersionNumber(const std::string& path) {
  // Match pattern: v<number>.metadata.json at end of path
  std::regex version_regex(R"(v(\d+)\.metadata\.json$)");
  std::smatch match;
  if (std::regex_search(path, match, version_regex)) {
    return std::stoi(match[1].str());
  }
  return -1;
}

}  // namespace

IcebergTableMetadata parseMetadataJson(const std::string& json_content) {
  rapidjson::Document doc;
  doc.Parse(json_content.c_str());

  if (doc.HasParseError()) {
    THROW_EXCEPTION_WITH_FILE_LINE(
        "[iceberg] Failed to parse metadata JSON: " +
        std::string(rapidjson::GetParseError_En(doc.GetParseError())));
  }

  IcebergTableMetadata metadata;

  // format-version (required)
  if (!doc.HasMember("format-version") || !doc["format-version"].IsInt()) {
    THROW_EXCEPTION_WITH_FILE_LINE(
        "[iceberg] metadata.json missing required field 'format-version'");
  }
  metadata.format_version = doc["format-version"].GetInt();

  // table-uuid (optional)
  if (doc.HasMember("table-uuid") && doc["table-uuid"].IsString()) {
    metadata.table_uuid = doc["table-uuid"].GetString();
  }

  // location (required)
  if (!doc.HasMember("location") || !doc["location"].IsString()) {
    THROW_EXCEPTION_WITH_FILE_LINE(
        "[iceberg] metadata.json missing required field 'location'");
  }
  metadata.location = doc["location"].GetString();

  // schema (required) — parse fields array
  if (!doc.HasMember("schema") || !doc["schema"].IsObject()) {
    THROW_EXCEPTION_WITH_FILE_LINE(
        "[iceberg] metadata.json missing required field 'schema'");
  }
  const auto& schema = doc["schema"];
  if (!schema.HasMember("fields") || !schema["fields"].IsArray()) {
    THROW_EXCEPTION_WITH_FILE_LINE(
        "[iceberg] metadata.json schema missing 'fields' array");
  }
  for (const auto& field : schema["fields"].GetArray()) {
    IcebergField f;
    f.id = field.HasMember("id") ? field["id"].GetInt() : 0;
    f.name = field.HasMember("name") ? field["name"].GetString() : "";
    f.required =
        field.HasMember("required") ? field["required"].GetBool() : false;
    // Type can be a string or an object (for nested types)
    if (field.HasMember("type")) {
      if (field["type"].IsString()) {
        f.type = field["type"].GetString();
      } else if (field["type"].IsObject()) {
        // Nested type: extract the "type" field from the object
        if (field["type"].HasMember("type") &&
            field["type"]["type"].IsString()) {
          f.type = field["type"]["type"].GetString();
        } else {
          f.type = "struct";  // default to struct for complex objects
        }
      }
    }
    metadata.schema_fields.push_back(std::move(f));
  }

  // current-snapshot-id (required for non-empty tables)
  if (doc.HasMember("current-snapshot-id")) {
    if (doc["current-snapshot-id"].IsInt64()) {
      metadata.current_snapshot_id = doc["current-snapshot-id"].GetInt64();
    } else {
      metadata.current_snapshot_id = -1;  // null or invalid
    }
  } else {
    metadata.current_snapshot_id = -1;
  }

  // snapshots (optional — may be empty for brand-new tables)
  if (doc.HasMember("snapshots") && doc["snapshots"].IsArray()) {
    for (const auto& snap : doc["snapshots"].GetArray()) {
      IcebergSnapshot snapshot;
      snapshot.snapshot_id =
          snap.HasMember("snapshot-id") ? snap["snapshot-id"].GetInt64() : 0;
      snapshot.timestamp_ms =
          snap.HasMember("timestamp-ms") ? snap["timestamp-ms"].GetInt64() : 0;
      snapshot.manifest_list = snap.HasMember("manifest-list")
                                   ? snap["manifest-list"].GetString()
                                   : "";
      if (snap.HasMember("summary") && snap["summary"].IsObject()) {
        for (auto it = snap["summary"].MemberBegin();
             it != snap["summary"].MemberEnd(); ++it) {
          if (it->value.IsString()) {
            snapshot.summary[it->name.GetString()] = it->value.GetString();
          }
        }
      }
      metadata.snapshots.push_back(std::move(snapshot));
    }
  }

  return metadata;
}

std::string findLatestMetadataFile(const std::string& table_path,
                                   fsys::FileSystem* fs) {
  std::string glob_pattern = table_path + "/metadata/v*.metadata.json";
  auto files = fs->glob(glob_pattern);

  if (files.empty()) {
    THROW_IO_EXCEPTION(
        "[iceberg] No metadata files found at '" + table_path +
        "/metadata/'. Ensure this is a valid Iceberg table directory.");
  }

  // Sort by version number, pick highest
  std::sort(files.begin(), files.end(),
            [](const std::string& a, const std::string& b) {
              return extractVersionNumber(a) < extractVersionNumber(b);
            });

  return files.back();
}

IcebergTableMetadata readTableMetadata(const std::string& table_path,
                                       fsys::FileSystem* fs) {
  std::string metadata_path = findLatestMetadataFile(table_path, fs);
  auto arrow_fs = fs->toArrowFileSystem();
  std::string content = readFileContent(metadata_path, arrow_fs.get());
  return parseMetadataJson(content);
}

}  // namespace iceberg
}  // namespace neug
