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
#include <new>
#include <string>
#include <vector>

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
using apsara::odps::sdk::max_storage_api::MaxStorageApi;

// Concrete definition of the opaque handle. Owns the SDK objects; both live
// entirely on the ABI=0 side and are never exposed as C++ types.
struct Connection {
  MaxStorageApi api;
  IODPSPtr odps;
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

// The public seam uses C linkage; the handle type is the file-local Connection.
struct OdpsGlueConnection : public Connection {};

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
