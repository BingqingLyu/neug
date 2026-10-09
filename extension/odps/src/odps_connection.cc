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

#include "odps_connection.h"

#include <glog/logging.h>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <string>

#include "neug/utils/exception/exception.h"
#include "odps_error.h"

#if defined(ODPS_SDK_ENABLE_ARROW)
#include "odps_sdk_glue.h"
#endif

namespace neug {
namespace extension {
namespace odps {
namespace {

// Look up the first non-empty value among the given option keys.
std::string findFirstOption(const reader::options_t& options,
                            std::initializer_list<const char*> keys) {
  for (const char* key : keys) {
    auto it = options.find(key);
    if (it != options.end() && !it->second.empty()) {
      return it->second;
    }
  }
  return "";
}

// Look up the first non-empty value among the given environment variables.
std::string findFirstEnv(std::initializer_list<const char*> envKeys) {
  for (const char* key : envKeys) {
    const char* v = std::getenv(key);
    if (v && std::strlen(v) > 0) {
      return v;
    }
  }
  return "";
}

// Resolve one setting: explicit options > environment > empty.
std::string resolveOption(const reader::options_t& options,
                          std::initializer_list<const char*> optionKeys,
                          std::initializer_list<const char*> envKeys) {
  std::string value = findFirstOption(options, optionKeys);
  if (!value.empty()) {
    return value;
  }
  return findFirstEnv(envKeys);
}

}  // namespace

std::string maskCredential(const std::string& value) {
  if (value.empty()) {
    return "(not set)";
  }
  if (value.size() <= 4) {
    return "***";
  }
  return value.substr(0, 4) + "***";
}

OdpsConnectionOptions OdpsConnectionOptionsBuilder::build() const {
  const auto& options = schema_.options;
  OdpsConnectionOptions conn;

  // Credentials: options (access_id/access_key + *_id/_secret aliases) > env.
  conn.accessId = resolveOption(
      options,
      {OdpsConnectionKeys::kAccessId, OdpsConnectionKeys::kAccessKeyId},
      {OdpsConnectionKeys::kEnvAccessKeyId,
       OdpsConnectionKeys::kEnvAccessKeyIdAlias});
  conn.accessKey = resolveOption(
      options,
      {OdpsConnectionKeys::kAccessKey, OdpsConnectionKeys::kAccessKeySecret},
      {OdpsConnectionKeys::kEnvAccessKeySecret,
       OdpsConnectionKeys::kEnvAccessKeySecretAlias});

  // Network: configurable endpoint (VPC intranet preferred, not enforced).
  conn.endpoint = resolveOption(options, {OdpsConnectionKeys::kEndpoint},
                                {OdpsConnectionKeys::kEnvEndpoint});
  conn.tunnelEndpoint =
      resolveOption(options, {OdpsConnectionKeys::kTunnelEndpoint},
                    {OdpsConnectionKeys::kEnvTunnelEndpoint});
  conn.project = resolveOption(options, {OdpsConnectionKeys::kProject},
                               {OdpsConnectionKeys::kEnvProject});
  conn.quotaName = resolveOption(options, {OdpsConnectionKeys::kQuotaName},
                                 {OdpsConnectionKeys::kEnvQuotaName});
  conn.regionId = resolveOption(options, {OdpsConnectionKeys::kRegionId},
                                {OdpsConnectionKeys::kEnvRegionId});

  // Fail loudly: ODPS has no anonymous / silent-default access path.
  if (!conn.hasCredentials()) {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "odps_connection: no credentials resolved. Provide access_id and "
        "access_key options, or set ODPS_ACCESS_KEY_ID/ODPS_ACCESS_KEY_SECRET "
        "(aliases: "
        "ALIBABA_CLOUD_ACCESS_KEY_ID/ALIBABA_CLOUD_ACCESS_KEY_SECRET).");
  }
  if (conn.endpoint.empty()) {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "odps_connection: no endpoint resolved. Provide the 'endpoint' option "
        "or set ODPS_ENDPOINT. Note: MaxCompute service endpoints are "
        "typically "
        "reachable only from within the same VPC/region.");
  }

  LOG(INFO) << "=== OdpsConnectionOptions ===";
  LOG(INFO) << "  Endpoint: " << conn.endpoint;
  LOG(INFO) << "  Tunnel endpoint: "
            << (conn.tunnelEndpoint.empty() ? "(default)"
                                            : conn.tunnelEndpoint);
  LOG(INFO) << "  Default project: "
            << (conn.project.empty() ? "(none)" : conn.project);
  LOG(INFO) << "  Quota: "
            << (conn.quotaName.empty() ? "(default)" : conn.quotaName);
  LOG(INFO) << "  Access id: " << maskCredential(conn.accessId);
  LOG(INFO) << "  Access key: " << maskCredential(conn.accessKey);
  LOG(INFO) << "=============================";

  return conn;
}

// ============================================================================
// SDK connection handle (only real when built with NEUG_WITH_ODPS_SDK)
//
// The handle is an opaque OdpsGlueConnection* owned by the inner ABI=0 glue
// library (glue/odps_sdk_glue.h). This ABI=1 file only stores and forwards the
// raw pointer, so no SDK/Arrow type ever appears here.
// ============================================================================

struct OdpsConnection::Impl {
  // Opaque glue handle; nullptr when built without the SDK.
  void* glue = nullptr;
#if defined(ODPS_SDK_ENABLE_ARROW)
  ~Impl() {
    if (glue != nullptr) {
      odps_glue_disconnect(static_cast<OdpsGlueConnection*>(glue));
      glue = nullptr;
    }
  }
#endif
};

OdpsConnection::OdpsConnection(const OdpsConnectionOptions& options)
    : options_(options), impl_(std::make_unique<Impl>()) {
#if defined(ODPS_SDK_ENABLE_ARROW)
  OdpsGlueConfig config;
  config.access_id = options_.accessId.c_str();
  config.access_key = options_.accessKey.c_str();
  config.endpoint = options_.endpoint.c_str();
  // Empty strings are fine: the glue treats "" the same as "not set".
  config.tunnel_endpoint = options_.tunnelEndpoint.c_str();
  config.project = options_.project.c_str();
  config.quota_name = options_.quotaName.c_str();
  config.region_id = options_.regionId.c_str();

  char* error = nullptr;
  impl_->glue = odps_glue_connect(&config, &error);
  if (impl_->glue == nullptr) {
    const std::string message = (error != nullptr) ? error : "unknown error";
    odps_glue_free_string(error);
    OdpsError::throwAttributed(
        "odps_connection: failed to initialize the ODPS connection", message,
        {options_.accessId, options_.accessKey});
  }
#endif
}

OdpsConnection::~OdpsConnection() = default;
OdpsConnection::OdpsConnection(OdpsConnection&&) noexcept = default;
OdpsConnection& OdpsConnection::operator=(OdpsConnection&&) noexcept = default;

void* OdpsConnection::glueHandle() const {
#if defined(ODPS_SDK_ENABLE_ARROW)
  if (impl_) {
    return impl_->glue;
  }
#endif
  return nullptr;
}

}  // namespace odps
}  // namespace extension
}  // namespace neug
