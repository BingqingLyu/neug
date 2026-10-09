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

#include "odps_error.h"

#include <algorithm>
#include <cctype>
#include <initializer_list>

#include "neug/utils/exception/exception.h"
#include "odps_connection.h"

namespace neug {
namespace extension {
namespace odps {
namespace {

std::string toLower(const std::string& value) {
  std::string result = value;
  std::transform(result.begin(), result.end(), result.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return result;
}

bool containsAny(const std::string& haystack,
                 std::initializer_list<const char*> needles) {
  for (const char* needle : needles) {
    if (haystack.find(needle) != std::string::npos) {
      return true;
    }
  }
  return false;
}

// Actionable, user-facing hint for each category. Kept separate from
// categoryName() (a short log/test label) so the wording can be verbose.
const char* categoryHint(OdpsErrorCategory category) {
  switch (category) {
  case OdpsErrorCategory::kAuthentication:
    return "authentication or authorization failed -- verify the AccessKey "
           "ID/secret and that the account has Storage API permission on this "
           "project/table";
  case OdpsErrorCategory::kNotFound:
    return "the project, schema or table does not exist, or the current "
           "credentials have no access to it";
  case OdpsErrorCategory::kNetwork:
    return "the ODPS endpoint could not be reached -- check ODPS_ENDPOINT and "
           "network/VPC reachability (MaxCompute endpoints are usually "
           "region/VPC restricted)";
  case OdpsErrorCategory::kSessionTimeout:
    return "the ODPS request or read session timed out or expired -- the table "
           "may be large or the service busy; retry, or tune split_size_mb";
  case OdpsErrorCategory::kQuota:
    return "ODPS Storage API quota or rate limit exceeded -- check the "
           "quota_name / resource group";
  case OdpsErrorCategory::kBadRequest:
    return "ODPS rejected the request as malformed -- check the odps:// "
           "address, project/schema/table and partition spec";
  case OdpsErrorCategory::kUnknown:
  default:
    return "the ODPS request failed";
  }
}

}  // namespace

std::string OdpsError::extractCode(const std::string& glueMessage) {
  const auto open = glueMessage.find('[');
  if (open == std::string::npos) {
    return "";
  }
  const auto close = glueMessage.find(']', open);
  if (close == std::string::npos) {
    return "";
  }
  return glueMessage.substr(open + 1, close - open - 1);
}

OdpsErrorCategory OdpsError::classify(const std::string& glueMessage) {
  // Match on the whole (lower-cased) message so classification still works when
  // the glue fell back to a bare std::exception::what() with no "[code]"
  // prefix. Order matters: more specific causes are tested before the generic
  // network/timeout buckets (e.g. "ConnectTimeout" -> Network, not Timeout).
  //
  // Whitespace is stripped as well so a keyword matches whether the glue gave a
  // CamelCase error code ("NetworkUnreachable") or space-separated prose from a
  // bare what() ("could not resolve host").
  std::string text = toLower(glueMessage);
  text.erase(std::remove_if(text.begin(), text.end(),
                            [](unsigned char c) { return std::isspace(c); }),
             text.end());

  if (containsAny(text, {"accessdenied", "unauthorized", "notauthorized",
                         "forbidden", "invalidaccesskey", "accesskeyidnotfound",
                         "signaturedoesnotmatch", "signaturenonceused",
                         "invalidtoken", "tokenexpired", "expiredtoken",
                         "authentication", "unauthenticated", "authfail",
                         "nopermission", "permissiondenied",
                         "invalidcredential", "odps-0410", "odps-0060"})) {
    return OdpsErrorCategory::kAuthentication;
  }
  if (containsAny(text, {"nosuch", "tablenotfound", "projectnotfound",
                         "notfound", "doesnotexist", "invalidtable",
                         "invalidproject", "missingtable", "missingproject",
                         "odps-0130131", "odps-0110061"})) {
    return OdpsErrorCategory::kNotFound;
  }
  if (containsAny(text, {"quota", "flowexceeded", "flowcontrol", "throttl",
                         "toomanyrequests", "ratelimit", "requestthrottled",
                         "resourcenotenough", "limitexceeded", "odps-0420"})) {
    return OdpsErrorCategory::kQuota;
  }
  if (containsAny(text, {"connect", "couldnotresolve", "resolvehost",
                         "networkunreachable", "network", "socket", "curl",
                         "hostnotfound", "unreachable", "dns", "noroutetohost",
                         "endpointinvalid"})) {
    return OdpsErrorCategory::kNetwork;
  }
  if (containsAny(text, {"timeout", "timedout", "deadlineexceeded",
                         "sessionexpired", "sessiontimeout", "requesttimeout",
                         "readtimeout", "odps-0520"})) {
    return OdpsErrorCategory::kSessionTimeout;
  }
  if (containsAny(text, {"invalidargument", "invalidparameter",
                         "invalidpartition", "malformed", "parseerror",
                         "semantic", "syntaxerror", "invalidrequest",
                         "badrequest", "invalidquery", "odps-0130161"})) {
    return OdpsErrorCategory::kBadRequest;
  }
  return OdpsErrorCategory::kUnknown;
}

const char* OdpsError::categoryName(OdpsErrorCategory category) {
  switch (category) {
  case OdpsErrorCategory::kAuthentication:
    return "Authentication";
  case OdpsErrorCategory::kNotFound:
    return "NotFound";
  case OdpsErrorCategory::kNetwork:
    return "Network";
  case OdpsErrorCategory::kSessionTimeout:
    return "SessionTimeout";
  case OdpsErrorCategory::kQuota:
    return "Quota";
  case OdpsErrorCategory::kBadRequest:
    return "BadRequest";
  case OdpsErrorCategory::kUnknown:
  default:
    return "Unknown";
  }
}

std::string OdpsError::attribute(const std::string& operation,
                                 const std::string& glueMessage) {
  // Preserve the raw glue text (it already carries "[code] msg") for diagnosis,
  // prefixed by the actionable hint so the cause is locatable at a glance.
  return operation + ": " + categoryHint(classify(glueMessage)) +
         " (ODPS reported: " + glueMessage + ")";
}

std::string OdpsError::redactSecrets(const std::string& message,
                                     const std::vector<std::string>& secrets) {
  std::string result = message;
  for (const auto& secret : secrets) {
    // Skip short values so ordinary words are never masked by accident; real
    // AccessKey ids/secrets are far longer than this.
    if (secret.size() < 4) {
      continue;
    }
    const std::string masked = maskCredential(secret);
    size_t pos = 0;
    while ((pos = result.find(secret, pos)) != std::string::npos) {
      result.replace(pos, secret.size(), masked);
      pos += masked.size();
    }
  }
  return result;
}

void OdpsError::throwAttributed(const std::string& operation,
                                const std::string& glueMessage,
                                const std::vector<std::string>& secrets) {
  const std::string redacted = redactSecrets(glueMessage, secrets);
  const std::string message = attribute(operation, redacted);
  switch (classify(redacted)) {
  case OdpsErrorCategory::kAuthentication:
    THROW_PERMISSION_DENIED(message);
  case OdpsErrorCategory::kNotFound:
    THROW_NOT_FOUND_EXCEPTION(message);
  case OdpsErrorCategory::kNetwork:
    THROW_CONNECTION_EXCEPTION(message);
  case OdpsErrorCategory::kBadRequest:
    THROW_INVALID_ARGUMENT_EXCEPTION(message);
  case OdpsErrorCategory::kSessionTimeout:
  case OdpsErrorCategory::kQuota:
  case OdpsErrorCategory::kUnknown:
  default:
    THROW_IO_EXCEPTION(message);
  }
}

}  // namespace odps
}  // namespace extension
}  // namespace neug
