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

#include "odps_options.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "neug/utils/exception/exception.h"

namespace neug {
namespace extension {
namespace odps {
namespace {

std::string trim(const std::string& s) {
  size_t begin = 0;
  size_t end = s.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(s[begin]))) {
    ++begin;
  }
  while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1]))) {
    --end;
  }
  return s.substr(begin, end - begin);
}

bool hasOdpsScheme(const std::string& s) {
  constexpr size_t kLen = 7;  // strlen("odps://")
  if (s.size() < kLen) {
    return false;
  }
  for (size_t i = 0; i < kLen; ++i) {
    if (std::tolower(static_cast<unsigned char>(s[i])) != kOdpsScheme[i]) {
      return false;
    }
  }
  return true;
}

// Split `authority` (portion before `?`) on '.' into at most 3 segments and
// fill project/schema/table. Any empty segment is a hard error.
void splitAuthority(const std::string& authority, OdpsSourceDesc& desc,
                    const std::string& source) {
  std::vector<std::string> parts;
  size_t start = 0;
  while (true) {
    size_t dot = authority.find('.', start);
    if (dot == std::string::npos) {
      parts.push_back(trim(authority.substr(start)));
      break;
    }
    parts.push_back(trim(authority.substr(start, dot - start)));
    start = dot + 1;
  }

  if (parts.empty() || parts.size() > 3) {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "odps_options: invalid table identifier '" + source +
        "'. Expected 'odps://[project.][schema.]table', got " +
        std::to_string(parts.size()) + " dot-separated segment(s).");
  }

  auto nonEmpty = [&](const std::string& v, const char* what) {
    if (v.empty()) {
      THROW_INVALID_ARGUMENT_EXCEPTION(
          "odps_options: empty " + std::string(what) + " in '" + source + "'.");
    }
  };

  if (parts.size() == 1) {
    // table
    nonEmpty(parts[0], "table");
    desc.table = parts[0];
  } else if (parts.size() == 2) {
    // project.table
    nonEmpty(parts[0], "project");
    nonEmpty(parts[1], "table");
    desc.project = parts[0];
    desc.table = parts[1];
  } else {  // parts.size() == 3
    // project.schema.table
    nonEmpty(parts[0], "project");
    nonEmpty(parts[1], "schema");
    nonEmpty(parts[2], "table");
    desc.project = parts[0];
    desc.schema = parts[1];
    desc.table = parts[2];
  }
}

// Parse a `k=v[,k=v...]` partition spec into ordered "k=v" strings.
std::vector<std::string> parsePartitions(const std::string& spec,
                                         const std::string& source) {
  std::vector<std::string> out;
  if (trim(spec).empty()) {
    return out;
  }
  size_t start = 0;
  while (start <= spec.size()) {
    size_t sep = spec.find_first_of(",&", start);
    std::string term = trim(spec.substr(
        start, sep == std::string::npos ? std::string::npos : sep - start));
    if (term.empty()) {
      THROW_INVALID_ARGUMENT_EXCEPTION(
          "odps_options: empty partition term in '" + source +
          "'. Each partition must be 'key=value'.");
    }
    size_t eq = term.find('=');
    if (eq == std::string::npos) {
      THROW_INVALID_ARGUMENT_EXCEPTION(
          "odps_options: invalid partition term '" + term + "' in '" + source +
          "'. Expected 'key=value'.");
    }
    std::string key = trim(term.substr(0, eq));
    std::string value = trim(term.substr(eq + 1));
    if (key.empty() || value.empty()) {
      THROW_INVALID_ARGUMENT_EXCEPTION(
          "odps_options: invalid partition term '" + term + "' in '" + source +
          "'. Both key and value must be "
          "non-empty ('key=value').");
    }
    out.push_back(key + "=" + value);
    if (sep == std::string::npos) {
      break;
    }
    start = sep + 1;
  }
  return out;
}

}  // namespace

OdpsSourceDesc OdpsOptions::parseAddress(const std::string& address) {
  std::string s = trim(address);
  if (s.empty()) {
    THROW_INVALID_ARGUMENT_EXCEPTION("odps_options: empty ODPS address.");
  }
  if (hasOdpsScheme(s)) {
    s = s.substr(7);  // strlen("odps://")
  }

  OdpsSourceDesc desc;
  std::string authority = s;
  std::string partitionSpec;
  size_t qmark = s.find('?');
  if (qmark != std::string::npos) {
    authority = s.substr(0, qmark);
    partitionSpec = s.substr(qmark + 1);
  }

  splitAuthority(authority, desc, address);
  desc.partitions = parsePartitions(partitionSpec, address);
  return desc;
}

OdpsSourceDesc OdpsOptions::fromFileSchema(const reader::FileSchema& schema) {
  const auto& options = schema.options;

  OdpsSourceDesc desc;
  if (!schema.paths.empty()) {
    desc = parseAddress(schema.paths[0]);
  } else {
    // No URL: everything must come from explicit options; table is required.
    desc = OdpsSourceDesc{};
  }

  auto findOpt = [&](const char* key) -> std::string {
    auto it = options.find(key);
    return it == options.end() ? std::string() : trim(it->second);
  };

  if (!desc.hasProject()) {
    desc.project = findOpt(OdpsConfigOptionKeys::kProject);
  }
  if (desc.schema == kOdpsDefaultSchema) {
    std::string optSchema = findOpt(OdpsConfigOptionKeys::kSchema);
    if (!optSchema.empty()) {
      desc.schema = optSchema;
    }
  }
  if (desc.table.empty()) {
    desc.table = findOpt(OdpsConfigOptionKeys::kTable);
  }
  if (desc.partitions.empty()) {
    std::string optParts = findOpt(OdpsConfigOptionKeys::kPartitions);
    if (!optParts.empty()) {
      // Reuse the same grammar, prefixing a dummy scheme so parsePartitions
      // reports the option value on error.
      desc.partitions = parsePartitions(optParts, "partitions option");
    }
  }
  if (!desc.hasQuota()) {
    desc.quotaName = findOpt(OdpsConfigOptionKeys::kQuotaName);
  }
  if (!desc.hasSplitSize()) {
    std::string split = findOpt(OdpsConfigOptionKeys::kSplitSizeMb);
    if (!split.empty()) {
      try {
        desc.splitSizeMb = std::stol(split);
      } catch (const std::exception&) {
        THROW_INVALID_ARGUMENT_EXCEPTION(
            "odps_options: invalid split_size_mb '" + split +
            "'. Expected a non-negative integer (MiB).");
      }
      if (desc.splitSizeMb < 0) {
        THROW_INVALID_ARGUMENT_EXCEPTION(
            "odps_options: split_size_mb must be non-negative, got '" + split +
            "'.");
      }
    }
  }

  if (desc.table.empty()) {
    THROW_INVALID_ARGUMENT_EXCEPTION(
        "odps_options: no table specified. Provide an "
        "'odps://[project.][schema.]table' path or a 'table' option.");
  }
  return desc;
}

}  // namespace odps
}  // namespace extension
}  // namespace neug
