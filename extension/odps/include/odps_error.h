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

#include <string>
#include <vector>

namespace neug {
namespace extension {
namespace odps {

/**
 * @brief Coarse, user-facing classes an ODPS failure can be attributed to.
 *
 * The inner glue reports SDK failures as a plain "[ErrorCode] message" C
 * string (see glue/odps_sdk_glue.h). Spec FR-005/SC-006 and plan algorithm 6
 * require turning that raw code into a clear, locatable cause -- and never
 * echoing credentials. This enum is the attribution target: each value maps to
 * a distinct hint and (for the actionable ones) a distinct NeuG exception type.
 */
enum class OdpsErrorCategory {
  kUnknown = 0,
  kAuthentication,  // bad/missing AccessKey, signature, or no permission
  kNotFound,        // project / schema / table / partition does not exist
  kNetwork,         // endpoint unreachable, DNS/connection/socket failure
  kSessionTimeout,  // request or read-session timeout / expiry
  kQuota,           // Storage API quota or throttling exceeded
  kBadRequest,      // SDK rejected the address/parameters as malformed
};

/**
 * @brief SDK-free attribution of glue error strings to actionable causes.
 *
 * Pure string logic over the glue's "[ErrorCode] message" format, so it is
 * fully unit-testable without the ODPS SDK (mirrors the schema-converter and
 * Arrow-bridge layers). No SDK/Arrow type appears here.
 */
class OdpsError {
 public:
  // Extract the bracketed error code from a glue message ("[AccessDenied] ..."
  // -> "AccessDenied"); returns an empty string when there is no such prefix.
  static std::string extractCode(const std::string& glueMessage);

  // Classify a glue message by its error code (falling back to keywords in the
  // whole message). Never throws; unmatched input yields kUnknown.
  static OdpsErrorCategory classify(const std::string& glueMessage);

  // Human-readable category name, for logs and tests.
  static const char* categoryName(OdpsErrorCategory category);

  // Build a locatable, actionable message: "<operation>: <hint> (ODPS error
  // <code>: <original>)". The original glue text is preserved for diagnosis.
  static std::string attribute(const std::string& operation,
                               const std::string& glueMessage);

  // Replace any occurrence of the given secrets (e.g. an AccessKey) with a
  // masked form, so a raw SDK message that happens to echo a credential can
  // never leak it (spec SC-006: zero plaintext-credential disclosure).
  static std::string redactSecrets(const std::string& message,
                                   const std::vector<std::string>& secrets);

  // Convenience: redact, attribute and throw the NeuG exception that matches
  // the category (authentication -> PermissionDenied, not-found -> NotFound,
  // network -> Connection, bad-request -> InvalidArgument, else IO). Marked
  // [[noreturn]] so callers need no dead-code path after it.
  [[noreturn]] static void throwAttributed(
      const std::string& operation, const std::string& glueMessage,
      const std::vector<std::string>& secrets = {});
};

}  // namespace odps
}  // namespace extension
}  // namespace neug
