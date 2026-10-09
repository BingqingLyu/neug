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

// Inner ABI=0 glue: the ONLY translation unit in the odps extension that
// includes the ODPS SDK / Arrow headers. Compiled with
// -D_GLIBCXX_USE_CXX11_ABI=0 to match the SDK; it exposes the pure C seam
// declared in odps_sdk_glue.h so no std::string/STL object ever crosses into
// the ABI=1 outer extension. This file must not include any NeuG core header.

#include "odps_sdk_glue.h"

#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <new>
#include <string>
#include <vector>

#include <arrow/api.h>
#include <arrow/c/bridge.h>

#include "configuration.h"
#include "max_storage_api.h"
#include "odps_api.h"
#include "odps_exception.h"
#include "odps_table.h"
#include "odps_types.h"

namespace {

using apsara::odps::sdk::AliyunAccount;
using apsara::odps::sdk::Configuration;
using apsara::odps::sdk::IODPS;
using apsara::odps::sdk::IODPSPtr;
using apsara::odps::sdk::IODPSTableColumn;
using apsara::odps::sdk::IODPSTablePtr;
using apsara::odps::sdk::IODPSTableSchemaPtr;
using apsara::odps::sdk::OdpsException;
using apsara::odps::sdk::max_storage_api::FilterOptions;
using apsara::odps::sdk::max_storage_api::IArrowReadStreamPtr;
using apsara::odps::sdk::max_storage_api::IRawPredicate;
using apsara::odps::sdk::max_storage_api::ISplitPtr;
using apsara::odps::sdk::max_storage_api::ISplitsPtr;
using apsara::odps::sdk::max_storage_api::ITableReadSessionBuilder;
using apsara::odps::sdk::max_storage_api::ITableReadSessionBuilderPtr;
using apsara::odps::sdk::max_storage_api::ITableReadSessionPtr;
using apsara::odps::sdk::max_storage_api::MaxStorageApi;
using apsara::odps::sdk::max_storage_api::ReadOptions;
using apsara::odps::sdk::max_storage_api::SplitMode;
using apsara::odps::sdk::max_storage_api::SplitOptions;

// Concrete definition of the opaque connection handle. Owns the SDK objects;
// both live entirely on the ABI=0 side and are never exposed as C++ types.
struct Connection {
  MaxStorageApi api;
  IODPSPtr odps;
};

// Concrete definition of the opaque reader handle. Owns the Storage API read
// session, its split list, the split cursor and the currently-open stream; all
// live entirely on the ABI=0 side and are never exposed as C++ types.
struct Reader {
  ITableReadSessionPtr session;
  ISplitsPtr splits;
  ReadOptions readOptions;
  int32_t splitCount = 0;
  int32_t splitIndex = 0;      // next split to open
  IArrowReadStreamPtr stream;  // currently-open stream (null between splits)
};

const char* orEmpty(const char* s) { return s ? s : ""; }

bool nonEmpty(const char* s) { return s && *s; }

// Copy a std::string into a malloc'd NUL-terminated C string (ABI-neutral).
char* dupString(const std::string& s) {
  char* p = static_cast<char*>(std::malloc(s.size() + 1));
  if (p != nullptr) {
    std::memcpy(p, s.c_str(), s.size() + 1);
  }
  return p;
}

std::string formatOdpsException(const OdpsException& e) {
  return std::string("[") + e.GetErrorCode() + "] " + e.GetErrorMsg();
}

}  // namespace

// The public seam uses C linkage; the handle types are the file-local structs.
struct OdpsGlueConnection : public Connection {};
struct OdpsGlueReader : public Reader {};

extern "C" OdpsGlueConnection* odps_glue_connect(const OdpsGlueConfig* config,
                                                 char** out_error) {
  if (out_error != nullptr) {
    *out_error = nullptr;
  }
  if (config == nullptr) {
    if (out_error != nullptr) {
      *out_error = dupString("odps_glue: null config");
    }
    return nullptr;
  }
  try {
    AliyunAccount account(orEmpty(config->access_id),
                          orEmpty(config->access_key));
    Configuration conf(account, orEmpty(config->endpoint));
    if (nonEmpty(config->tunnel_endpoint)) {
      conf.SetTunnelEndpoint(config->tunnel_endpoint);
    }
    if (nonEmpty(config->project)) {
      conf.SetDefaultProject(config->project);
    }
    if (nonEmpty(config->quota_name)) {
      conf.SetTunnelQuotaName(config->quota_name);
    }
    if (nonEmpty(config->region_id)) {
      conf.SetRegionId(config->region_id);
    }

    OdpsGlueConnection* conn = new OdpsGlueConnection();
    conn->api.Init(conf);
    // The core client reads authoritative table metadata (works even for empty
    // tables, unlike the Arrow read path).
    conn->odps = IODPS::Create(conf, orEmpty(config->project));
    return conn;
  } catch (const OdpsException& e) {
    if (out_error != nullptr) {
      *out_error = dupString(formatOdpsException(e));
    }
    return nullptr;
  } catch (const std::exception& e) {
    if (out_error != nullptr) {
      *out_error = dupString(e.what());
    }
    return nullptr;
  }
}

extern "C" void odps_glue_disconnect(OdpsGlueConnection* conn) { delete conn; }

extern "C" int odps_glue_sniff_schema(OdpsGlueConnection* conn,
                                      const char* project, const char* schema,
                                      const char* table, OdpsGlueSchema* out) {
  if (out != nullptr) {
    out->columns = nullptr;
    out->count = 0;
    out->error = nullptr;
  }
  if (conn == nullptr || out == nullptr) {
    if (out != nullptr) {
      out->error = dupString("odps_glue: null connection or output");
    }
    return -1;
  }
  try {
    IODPSTablePtr tablePtr = conn->odps->GetTables()->Get(
        orEmpty(project), orEmpty(schema), orEmpty(table));
    if (!tablePtr) {
      out->error =
          dupString(std::string("table not found: ") + orEmpty(project) + "." +
                    orEmpty(schema) + "." + orEmpty(table));
      return 1;
    }
    IODPSTableSchemaPtr tableSchema = tablePtr->GetSchema();
    if (!tableSchema) {
      out->error = dupString("empty schema returned for table");
      return 2;
    }

    // Only the data columns are surfaced; partition columns act as filters in
    // the Storage API read path and are not emitted as row columns in v1.
    const uint32_t count = tableSchema->GetColumnCount();
    std::vector<std::string> names;
    names.reserve(count);
    OdpsGlueColumn* arr = static_cast<OdpsGlueColumn*>(
        std::calloc(count > 0 ? count : 1, sizeof(OdpsGlueColumn)));
    if (arr == nullptr) {
      out->error = dupString("odps_glue: out of memory");
      return 3;
    }
    for (uint32_t i = 0; i < count; ++i) {
      const IODPSTableColumn& column = tableSchema->GetTableColumn(i);
      names.push_back(column.GetName());
      arr[i].name = dupString(names.back());
      arr[i].type_code = static_cast<int>(column.GetType());
      arr[i].nullable = column.GetNullable() ? 1 : 0;
    }
    out->columns = arr;
    out->count = count;
    return 0;
  } catch (const OdpsException& e) {
    out->error = dupString(formatOdpsException(e));
    return 4;
  } catch (const std::exception& e) {
    out->error = dupString(e.what());
    return 5;
  }
}

extern "C" void odps_glue_schema_free(OdpsGlueSchema* schema) {
  if (schema == nullptr) {
    return;
  }
  if (schema->columns != nullptr) {
    for (size_t i = 0; i < schema->count; ++i) {
      std::free(const_cast<char*>(schema->columns[i].name));
    }
    std::free(schema->columns);
    schema->columns = nullptr;
    schema->count = 0;
  }
  if (schema->error != nullptr) {
    std::free(schema->error);
    schema->error = nullptr;
  }
}

extern "C" void odps_glue_free_string(char* s) { std::free(s); }

extern "C" OdpsGlueReader* odps_glue_open_reader(
    OdpsGlueConnection* conn, const char* project, const char* schema,
    const char* table, const OdpsGlueReadOptions* options, char** out_error) {
  if (out_error != nullptr) {
    *out_error = nullptr;
  }
  if (conn == nullptr) {
    if (out_error != nullptr) {
      *out_error = dupString("odps_glue: null connection");
    }
    return nullptr;
  }
  try {
    // Default split mode is ROW_OFFSET; a positive split_size_mb hint switches
    // to SIZE mode with that target split size (module 3 tunes this further).
    SplitOptions splitOptions;
    if (options != nullptr && options->split_size_bytes > 0) {
      splitOptions.mSplitMode = SplitMode::SIZE;
      splitOptions.mSplitSize = options->split_size_bytes;
    }
    ReadOptions readOptions;
    if (options != nullptr && options->max_batch_rows > 0) {
      readOptions.mMaxBatchRows = options->max_batch_rows;
    }

    // Hold the builder's owning smart pointer for the whole chain:
    // BuildTableReadSession() returns it by value, and binding a bare reference
    // to *(temporary) would dangle once the temporary is destroyed.
    ITableReadSessionBuilderPtr builderPtr = conn->api.BuildTableReadSession();
    ITableReadSessionBuilder& builder = *builderPtr;
    builder.SetProject(orEmpty(project))
        .SetSchema(orEmpty(schema))
        .SetTable(orEmpty(table))
        .SetSplitOptions(splitOptions);
    // Session filter (module 3): column pruning (T301) + partition pruning
    // (T302) + predicate pushdown (T303), merged into one FilterOptions since
    // SetFilterOptions replaces the whole struct. Any part may be absent; the
    // call is made only when at least one is present, so a plain read leaves
    // FilterOptions unset and matches pre-pushdown behavior exactly.
    //
    // Column pruning: each caller-supplied name is a data column, given in
    // table-schema order, so it maps straight onto mRequiredDataColumns and the
    // session materializes only those columns.
    // Partition pruning: each caller-supplied spec is already a complete
    // '/'-delimited partition path, so it maps straight onto
    // mRequiredPartitions and the session reads only those partitions.
    // Predicate pushdown: the string is already in the SDK's
    // IPredicate::ToString() dialect (see odps_predicate_converter) and is
    // wrapped as a raw predicate for SetFilterPredicate. Engine-side filtering
    // still re-applies the full predicate, so this only reduces how many rows
    // are transferred.
    FilterOptions filterOptions;
    bool haveFilter = false;
    if (options != nullptr && options->required_data_columns != nullptr) {
      for (size_t i = 0; i < options->required_data_column_count; ++i) {
        if (nonEmpty(options->required_data_columns[i])) {
          filterOptions.mRequiredDataColumns.push_back(
              options->required_data_columns[i]);
        }
      }
      haveFilter = !filterOptions.mRequiredDataColumns.empty();
    }
    if (options != nullptr && options->required_partitions != nullptr) {
      for (size_t i = 0; i < options->required_partition_count; ++i) {
        if (nonEmpty(options->required_partitions[i])) {
          filterOptions.mRequiredPartitions.push_back(
              options->required_partitions[i]);
        }
      }
      haveFilter = haveFilter || !filterOptions.mRequiredPartitions.empty();
    }
    if (options != nullptr && nonEmpty(options->filter_predicate)) {
      filterOptions.mPredicate = IRawPredicate::Of(options->filter_predicate);
      haveFilter = true;
    }
    if (haveFilter) {
      builder.SetFilterOptions(filterOptions);
    }
    ITableReadSessionPtr session = builder.Build();
    if (!session) {
      if (out_error != nullptr) {
        *out_error = dupString("odps_glue: null read session");
      }
      return nullptr;
    }
    ISplitsPtr splits = session->GetSplits();
    if (!splits) {
      if (out_error != nullptr) {
        *out_error = dupString("odps_glue: null split list");
      }
      return nullptr;
    }

    OdpsGlueReader* reader = new OdpsGlueReader();
    reader->session = session;
    reader->splits = splits;
    reader->readOptions = readOptions;
    reader->splitCount = splits->GetSplitCount();
    reader->splitIndex = 0;
    return reader;
  } catch (const OdpsException& e) {
    if (out_error != nullptr) {
      *out_error = dupString(formatOdpsException(e));
    }
    return nullptr;
  } catch (const std::exception& e) {
    if (out_error != nullptr) {
      *out_error = dupString(e.what());
    }
    return nullptr;
  }
}

extern "C" long long odps_glue_reader_split_count(OdpsGlueReader* reader,
                                                  char** out_error) {
  if (out_error != nullptr) {
    *out_error = nullptr;
  }
  if (reader == nullptr) {
    if (out_error != nullptr) {
      *out_error = dupString("odps_glue: null reader");
    }
    return -1;
  }
  return static_cast<long long>(reader->splitCount);
}

extern "C" long long odps_glue_reader_record_count(OdpsGlueReader* reader,
                                                   char** out_error) {
  if (out_error != nullptr) {
    *out_error = nullptr;
  }
  if (reader == nullptr) {
    if (out_error != nullptr) {
      *out_error = dupString("odps_glue: null reader");
    }
    return -1;
  }
  try {
    return static_cast<long long>(reader->splits->GetRecordCount());
  } catch (const OdpsException& e) {
    if (out_error != nullptr) {
      *out_error = dupString(formatOdpsException(e));
    }
    return -1;
  } catch (const std::exception& e) {
    if (out_error != nullptr) {
      *out_error = dupString(e.what());
    }
    return -1;
  }
}

extern "C" int odps_glue_reader_next_batch(OdpsGlueReader* reader,
                                           void* out_array, void* out_schema,
                                           char** out_error) {
  if (out_error != nullptr) {
    *out_error = nullptr;
  }
  if (reader == nullptr || out_array == nullptr) {
    if (out_error != nullptr) {
      *out_error = dupString("odps_glue: null reader or output");
    }
    return -1;
  }
  try {
    while (true) {
      if (!reader->stream) {
        if (reader->splitIndex >= reader->splitCount) {
          return 0;  // end of data: every split exhausted
        }
        ISplitPtr split = reader->splits->GetSplit(reader->splitIndex);
        ++reader->splitIndex;
        reader->stream = reader->session->BuildTableReadStream()
                             ->SetSplit(split)
                             .SetReadOptions(reader->readOptions)
                             .Build();
        if (!reader->stream) {
          if (out_error != nullptr) {
            *out_error = dupString("odps_glue: failed to build read stream");
          }
          return -1;
        }
      }
      std::shared_ptr<arrow::RecordBatch> batch = reader->stream->Read();
      if (!batch) {
        // Current split drained; close it and advance to the next split.
        reader->stream->Close();
        reader->stream.reset();
        continue;
      }
      // Export through the Arrow C Data Interface (stable C ABI): the batch's
      // buffers stay owned by Arrow and are released by the consumer via the
      // release callbacks installed here. No Arrow C++ type crosses the seam.
      arrow::Status st = arrow::ExportRecordBatch(
          *batch, static_cast<struct ArrowArray*>(out_array),
          out_schema != nullptr ? static_cast<struct ArrowSchema*>(out_schema)
                                : nullptr);
      if (!st.ok()) {
        if (out_error != nullptr) {
          *out_error = dupString("odps_glue: failed to export record batch: " +
                                 st.ToString());
        }
        return -1;
      }
      return 1;
    }
  } catch (const OdpsException& e) {
    if (out_error != nullptr) {
      *out_error = dupString(formatOdpsException(e));
    }
    return -1;
  } catch (const std::exception& e) {
    if (out_error != nullptr) {
      *out_error = dupString(e.what());
    }
    return -1;
  }
}

extern "C" void odps_glue_reader_close(OdpsGlueReader* reader) {
  if (reader == nullptr) {
    return;
  }
  try {
    if (reader->stream) {
      reader->stream->Close();
    }
  } catch (...) {
    // Best-effort close; never let an exception cross the C seam.
  }
  delete reader;
}
