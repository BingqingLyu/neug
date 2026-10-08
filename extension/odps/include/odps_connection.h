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

#pragma once

#include <memory>
#include <string>

#include "neug/utils/io/read/common/options.h"
#include "neug/utils/io/read/common/schema.h"

namespace neug {
namespace extension {
namespace odps {

// Configuration option keys and environment variable names for ODPS access.
// Credential/endpoint resolution priority mirrors httpfs `s3_options.cc`:
//   explicit options > environment variables > error.
struct OdpsConnectionKeys {
  // Credentials (accepted as options; case-insensitive).
  static constexpr const char* kAccessId = "access_id";
  static constexpr const char* kAccessKeyId = "access_key_id";  // alias
  static constexpr const char* kAccessKey = "access_key";       // secret
  static constexpr const char* kAccessKeySecret = "access_key_secret";  // alias
  // Network / addressing.
  static constexpr const char* kEndpoint = "endpoint";
  static constexpr const char* kTunnelEndpoint = "tunnel_endpoint";
  static constexpr const char* kProject = "project";  // connection default
  static constexpr const char* kQuotaName = "quota_name";
  static constexpr const char* kRegionId = "region_id";

  // Environment variables (canonical + cloud-standard aliases).
  static constexpr const char* kEnvAccessKeyId = "ODPS_ACCESS_KEY_ID";
  static constexpr const char* kEnvAccessKeySecret = "ODPS_ACCESS_KEY_SECRET";
  static constexpr const char* kEnvAccessKeyIdAlias =
      "ALIBABA_CLOUD_ACCESS_KEY_ID";
  static constexpr const char* kEnvAccessKeySecretAlias =
      "ALIBABA_CLOUD_ACCESS_KEY_SECRET";
  static constexpr const char* kEnvEndpoint = "ODPS_ENDPOINT";
  static constexpr const char* kEnvTunnelEndpoint = "ODPS_TUNNEL_ENDPOINT";
  static constexpr const char* kEnvProject = "ODPS_PROJECT";
  static constexpr const char* kEnvQuotaName = "ODPS_QUOTA_NAME";
  static constexpr const char* kEnvRegionId = "ODPS_REGION_ID";
};

/**
 * @brief Resolved, credential-bearing connection configuration for ODPS.
 *
 * A plain value object produced once from FileSchema options + environment
 * variables. Unlike `OdpsSourceDesc` (T103), this is never serialized into
 * query text and is only used to initialize the SDK handle.
 */
struct OdpsConnectionOptions {
  std::string accessId;
  std::string accessKey;
  // ODPS service endpoint. Intranet (VPC) endpoints are preferred for v1 but
  // this is a plain configurable string — public/proxy/dedicated-line
  // endpoints work unchanged. Never hard-code "intranet only".
  std::string endpoint;
  std::string tunnelEndpoint;  // empty -> SDK routing / default
  std::string project;         // connection default (used when address omits)
  std::string quotaName;       // empty -> SDK default
  std::string regionId;        // optional

  bool hasCredentials() const {
    return !accessId.empty() && !accessKey.empty();
  }
};

// Mask a credential for safe logging: first 4 chars + "***", or "(not set)".
std::string maskCredential(const std::string& value);

/**
 * @brief Builds OdpsConnectionOptions from a FileSchema (options + env).
 *
 * Credentials and endpoint fail loudly when absent from both sources — there
 * is no silent anonymous fallback for ODPS (spec FR-004/005, SC-006).
 */
class OdpsConnectionOptionsBuilder {
 public:
  explicit OdpsConnectionOptionsBuilder(const reader::FileSchema& schema)
      : schema_(schema) {}

  OdpsConnectionOptions build() const;

 private:
  const reader::FileSchema& schema_;
};

/**
 * @brief Owns an initialized ODPS connection handle for reuse by sniff/read.
 *
 * Header is SDK-free: the connection is an opaque `OdpsGlueConnection*`
 * produced by the inner ABI=0 glue library (glue/odps_sdk_glue.h) and only
 * exists when the extension is built with `NEUG_WITH_ODPS_SDK` (which defines
 * `ODPS_SDK_ENABLE_ARROW`). Without the SDK the object still constructs (so
 * option resolution stays testable) but `glueHandle()` returns nullptr; the
 * reader then reports that reading requires the SDK. No SDK/Arrow type ever
 * crosses into this ABI=1 header.
 */
class OdpsConnection {
 public:
  explicit OdpsConnection(const OdpsConnectionOptions& options);
  ~OdpsConnection();

  OdpsConnection(const OdpsConnection&) = delete;
  OdpsConnection& operator=(const OdpsConnection&) = delete;
  OdpsConnection(OdpsConnection&&) noexcept;
  OdpsConnection& operator=(OdpsConnection&&) noexcept;

  const OdpsConnectionOptions& options() const { return options_; }

  // Opaque `OdpsGlueConnection*` handle owned by the inner ABI=0 glue library
  // (see glue/odps_sdk_glue.h), or nullptr when built without SDK support. The
  // outer extension never dereferences it — it is only handed back to the glue
  // functions (schema sniff now; data-plane read in T106), keeping every
  // SDK/Arrow type on the ABI=0 side of the seam.
  void* glueHandle() const;
  bool available() const { return glueHandle() != nullptr; }

 private:
  OdpsConnectionOptions options_;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace odps
}  // namespace extension
}  // namespace neug
