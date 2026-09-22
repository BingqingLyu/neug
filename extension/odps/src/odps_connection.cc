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

#if defined(ODPS_SDK_ENABLE_ARROW)
#include "configuration.h"
#include "max_storage_api.h"
#include "odps_api.h"
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
// SDK handle (only real when built with NEUG_WITH_ODPS_SDK)
// ============================================================================

struct OdpsConnection::Impl {
#if defined(ODPS_SDK_ENABLE_ARROW)
  apsara::odps::sdk::max_storage_api::MaxStorageApi api;
  // ODPS core client, used by schema sniffing (T105) to read authoritative
  // table metadata (works even for empty tables, unlike the Arrow read path).
  apsara::odps::sdk::IODPSPtr odps;
  bool ready = false;
#endif
};

OdpsConnection::OdpsConnection(const OdpsConnectionOptions& options)
    : options_(options), impl_(std::make_unique<Impl>()) {
#if defined(ODPS_SDK_ENABLE_ARROW)
  apsara::odps::sdk::AliyunAccount account(options_.accessId,
                                           options_.accessKey);
  apsara::odps::sdk::Configuration conf(
      account, options_.endpoint);  // NOLINT: SDK takes Account by value
  if (!options_.tunnelEndpoint.empty()) {
    conf.SetTunnelEndpoint(options_.tunnelEndpoint);
  }
  if (!options_.project.empty()) {
    conf.SetDefaultProject(options_.project);
  }
  if (!options_.quotaName.empty()) {
    conf.SetTunnelQuotaName(options_.quotaName);
  }
  if (!options_.regionId.empty()) {
    conf.SetRegionId(options_.regionId);
  }
  impl_->api.Init(conf);
  impl_->odps = apsara::odps::sdk::IODPS::Create(conf, options_.project);
  impl_->ready = true;
#endif
}

OdpsConnection::~OdpsConnection() = default;
OdpsConnection::OdpsConnection(OdpsConnection&&) noexcept = default;
OdpsConnection& OdpsConnection::operator=(OdpsConnection&&) noexcept = default;

void* OdpsConnection::handle() const {
#if defined(ODPS_SDK_ENABLE_ARROW)
  if (impl_ && impl_->ready) {
    return const_cast<apsara::odps::sdk::max_storage_api::MaxStorageApi*>(
        &impl_->api);
  }
#endif
  return nullptr;
}

void* OdpsConnection::odpsClient() const {
#if defined(ODPS_SDK_ENABLE_ARROW)
  if (impl_ && impl_->odps) {
    return impl_->odps.get();
  }
#endif
  return nullptr;
}

}  // namespace odps
}  // namespace extension
}  // namespace neug
