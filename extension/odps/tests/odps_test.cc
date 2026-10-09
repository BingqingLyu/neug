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

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "neug/common/types/value.h"
#include "neug/main/connection.h"
#include "neug/main/neug_db.h"
#include "neug/utils/exception/exception.h"

#include "odps_arrow_abi.h"
#include "odps_arrow_bridge.h"
#include "odps_connection.h"
#include "odps_error.h"
#include "odps_options.h"
#include "odps_schema_converter.h"

#if defined(__APPLE__)
#include <mach-o/dyld.h>
#else
#include <unistd.h>
#endif

namespace neug {
namespace extension {
namespace odps {
namespace {

// Save/clear a set of env vars for a test and restore them afterwards, so
// credential/endpoint resolution stays independent of the developer machine.
class ScopedOdpsEnv {
 public:
  explicit ScopedOdpsEnv(std::vector<const char*> keys)
      : keys_(std::move(keys)) {
    for (const char* key : keys_) {
      if (const char* prev = std::getenv(key)) {
        saved_.emplace_back(key, prev);
      }
      unsetenv(key);
    }
  }

  ~ScopedOdpsEnv() {
    // Fully restore the prior state: drop every key this scope touched (so a
    // value set() during the test never leaks into later tests), then re-apply
    // the ones that were present beforehand.
    for (const char* key : keys_) {
      unsetenv(key);
    }
    for (const auto& [key, value] : saved_) {
      setenv(key.c_str(), value.c_str(), 1);
    }
  }

  void set(const char* key, const char* value) { setenv(key, value, 1); }

 private:
  std::vector<const char*> keys_;
  std::vector<std::pair<std::string, std::string>> saved_;
};

std::vector<const char*> allOdpsEnvKeys() {
  return {OdpsConnectionKeys::kEnvAccessKeyId,
          OdpsConnectionKeys::kEnvAccessKeySecret,
          OdpsConnectionKeys::kEnvAccessKeyIdAlias,
          OdpsConnectionKeys::kEnvAccessKeySecretAlias,
          OdpsConnectionKeys::kEnvEndpoint,
          OdpsConnectionKeys::kEnvTunnelEndpoint,
          OdpsConnectionKeys::kEnvProject,
          OdpsConnectionKeys::kEnvQuotaName,
          OdpsConnectionKeys::kEnvRegionId};
}

// ============================================================================
// OdpsOptions::parseAddress
// ============================================================================

TEST(OdpsOptionsAddressTest, ParsesProjectSchemaTableAndPartition) {
  auto desc = OdpsOptions::parseAddress(
      "odps://my_project.default.my_table?pt=20260921");
  EXPECT_EQ(desc.project, "my_project");
  EXPECT_EQ(desc.schema, "default");
  EXPECT_EQ(desc.table, "my_table");
  ASSERT_EQ(desc.partitions.size(), 1u);
  EXPECT_EQ(desc.partitions[0], "pt=20260921");
}

TEST(OdpsOptionsAddressTest, ProjectAndTableOnlyUsesDefaultSchema) {
  auto desc = OdpsOptions::parseAddress("odps://proj.tbl");
  EXPECT_EQ(desc.project, "proj");
  EXPECT_EQ(desc.schema, "default");
  EXPECT_EQ(desc.table, "tbl");
  EXPECT_TRUE(desc.partitions.empty());
}

TEST(OdpsOptionsAddressTest, TableOnlyLeavesProjectEmpty) {
  auto desc = OdpsOptions::parseAddress("odps://orders");
  EXPECT_FALSE(desc.hasProject());
  EXPECT_EQ(desc.schema, "default");
  EXPECT_EQ(desc.table, "orders");
}

TEST(OdpsOptionsAddressTest, AcceptsAmpersandAndWhitespace) {
  auto desc = OdpsOptions::parseAddress(
      "  odps://sales.default.orders ? dt=20260921 & hh=01  ");
  EXPECT_EQ(desc.project, "sales");
  EXPECT_EQ(desc.table, "orders");
  ASSERT_EQ(desc.partitions.size(), 2u);
  EXPECT_EQ(desc.partitions[0], "dt=20260921");
  EXPECT_EQ(desc.partitions[1], "hh=01");
}

TEST(OdpsOptionsAddressTest, SchemeIsCaseInsensitiveAndOptional) {
  auto withScheme = OdpsOptions::parseAddress("ODPS://sales.default.orders");
  auto without = OdpsOptions::parseAddress("sales.default.orders");
  EXPECT_EQ(withScheme.table, without.table);
  EXPECT_EQ(withScheme.project, "sales");
  EXPECT_EQ(without.project, "sales");
}

TEST(OdpsOptionsAddressTest, RejectsTooManySegments) {
  EXPECT_THROW(OdpsOptions::parseAddress("odps://a.b.c.d"),
               exception::InvalidArgumentException);
}

TEST(OdpsOptionsAddressTest, RejectsEmptyTable) {
  EXPECT_THROW(OdpsOptions::parseAddress("odps://proj."),
               exception::InvalidArgumentException);
  EXPECT_THROW(OdpsOptions::parseAddress("odps://proj..tbl"),
               exception::InvalidArgumentException);
  EXPECT_THROW(OdpsOptions::parseAddress("odps://"),
               exception::InvalidArgumentException);
}

TEST(OdpsOptionsAddressTest, RejectsMalformedPartitions) {
  EXPECT_THROW(OdpsOptions::parseAddress("odps://proj.tbl?pt"),
               exception::InvalidArgumentException);
  EXPECT_THROW(OdpsOptions::parseAddress("odps://proj.tbl?=1"),
               exception::InvalidArgumentException);
  EXPECT_THROW(OdpsOptions::parseAddress("odps://proj.tbl?pt="),
               exception::InvalidArgumentException);
  EXPECT_THROW(OdpsOptions::parseAddress("odps://proj.tbl?pt=1,,ds=2"),
               exception::InvalidArgumentException);
}

// ============================================================================
// OdpsOptions::fromFileSchema
// ============================================================================

reader::FileSchema makeSchema(std::vector<std::string> paths,
                              reader::options_t options = {}) {
  reader::FileSchema schema;
  schema.paths = std::move(paths);
  schema.format = "odps";
  schema.protocol = "odps";
  schema.options = std::move(options);
  return schema;
}

TEST(OdpsOptionsSchemaTest, OptionsFillFieldsOmittedByAddress) {
  reader::options_t options;
  options["project"] = "sales";
  options["split_size_mb"] = "256";
  options["quota_name"] = "pay-as-you-go";
  auto schema = makeSchema({"odps://orders"}, options);
  auto desc = OdpsOptions::fromFileSchema(schema);
  EXPECT_EQ(desc.project, "sales");
  EXPECT_EQ(desc.table, "orders");
  EXPECT_EQ(desc.splitSizeMb, 256);
  EXPECT_EQ(desc.quotaName, "pay-as-you-go");
}

TEST(OdpsOptionsSchemaTest, AddressWinsOverOptions) {
  reader::options_t options;
  options["project"] = "from_option";
  options["schema"] = "from_option";
  auto schema = makeSchema({"odps://real_proj.real_schema.orders"}, options);
  auto desc = OdpsOptions::fromFileSchema(schema);
  EXPECT_EQ(desc.project, "real_proj");
  EXPECT_EQ(desc.schema, "real_schema");
}

TEST(OdpsOptionsSchemaTest, TableFromOptionsWhenNoPath) {
  reader::options_t options;
  options["project"] = "sales";
  options["table"] = "orders";
  options["partitions"] = "dt=20260921";
  auto schema = makeSchema({}, options);
  auto desc = OdpsOptions::fromFileSchema(schema);
  EXPECT_EQ(desc.project, "sales");
  EXPECT_EQ(desc.table, "orders");
  ASSERT_EQ(desc.partitions.size(), 1u);
  EXPECT_EQ(desc.partitions[0], "dt=20260921");
}

TEST(OdpsOptionsSchemaTest, RequiresTableSomewhere) {
  auto schema = makeSchema({}, {});
  EXPECT_THROW(OdpsOptions::fromFileSchema(schema),
               exception::InvalidArgumentException);
}

TEST(OdpsOptionsSchemaTest, RejectsInvalidSplitSize) {
  reader::options_t bad = {{"split_size_mb", "abc"}};
  auto schema = makeSchema({"odps://t"}, bad);
  EXPECT_THROW(OdpsOptions::fromFileSchema(schema),
               exception::InvalidArgumentException);

  reader::options_t neg = {{"split_size_mb", "-1"}};
  auto negSchema = makeSchema({"odps://t"}, neg);
  EXPECT_THROW(OdpsOptions::fromFileSchema(negSchema),
               exception::InvalidArgumentException);
}

// ============================================================================
// OdpsConnectionOptionsBuilder
// ============================================================================

TEST(OdpsConnectionTest, ResolvesCredentialsAndEndpointFromOptions) {
  ScopedOdpsEnv env(allOdpsEnvKeys());
  reader::options_t options;
  options["access_id"] = "my-access-id";
  options["access_key"] = "my-access-key";
  options["endpoint"] = "http://service.cn.maxcompute.aliyun-inc.com/api";
  options["project"] = "sales";
  options["quota_name"] = "pay-as-you-go";
  auto schema = makeSchema({"odps://sales.default.orders"}, options);

  auto conn = OdpsConnectionOptionsBuilder(schema).build();
  EXPECT_EQ(conn.accessId, "my-access-id");
  EXPECT_EQ(conn.accessKey, "my-access-key");
  EXPECT_EQ(conn.endpoint, "http://service.cn.maxcompute.aliyun-inc.com/api");
  EXPECT_EQ(conn.project, "sales");
  EXPECT_EQ(conn.quotaName, "pay-as-you-go");
  EXPECT_TRUE(conn.hasCredentials());
}

TEST(OdpsConnectionTest, AcceptsCredentialKeyAliases) {
  ScopedOdpsEnv env(allOdpsEnvKeys());
  reader::options_t options;
  options["access_key_id"] = "id-alias";
  options["access_key_secret"] = "secret-alias";
  options["endpoint"] = "ep";
  auto conn = OdpsConnectionOptionsBuilder(makeSchema({}, options)).build();
  EXPECT_EQ(conn.accessId, "id-alias");
  EXPECT_EQ(conn.accessKey, "secret-alias");
}

TEST(OdpsConnectionTest, FallsBackToEnvironment) {
  ScopedOdpsEnv env(allOdpsEnvKeys());
  env.set(OdpsConnectionKeys::kEnvAccessKeyId, "env-id");
  env.set(OdpsConnectionKeys::kEnvAccessKeySecret, "env-secret");
  env.set(OdpsConnectionKeys::kEnvEndpoint, "env-endpoint");
  reader::options_t options;
  options["access_id"] = "env-id";  // only endpoint has no option -> from env
  auto conn = OdpsConnectionOptionsBuilder(makeSchema({}, options)).build();
  EXPECT_EQ(conn.accessKey, "env-secret");
  EXPECT_EQ(conn.endpoint, "env-endpoint");
}

TEST(OdpsConnectionTest, SupportsAlibabaCloudEnvAliases) {
  ScopedOdpsEnv env(allOdpsEnvKeys());
  env.set(OdpsConnectionKeys::kEnvAccessKeyIdAlias, "cloud-id");
  env.set(OdpsConnectionKeys::kEnvAccessKeySecretAlias, "cloud-secret");
  env.set(OdpsConnectionKeys::kEnvEndpoint, "ep");
  auto conn = OdpsConnectionOptionsBuilder(makeSchema({}, {})).build();
  EXPECT_EQ(conn.accessId, "cloud-id");
  EXPECT_EQ(conn.accessKey, "cloud-secret");
}

TEST(OdpsConnectionTest, OptionsTakePrecedenceOverEnvironment) {
  ScopedOdpsEnv env(allOdpsEnvKeys());
  env.set(OdpsConnectionKeys::kEnvAccessKeyId, "env-id");
  env.set(OdpsConnectionKeys::kEnvEndpoint, "env-endpoint");
  reader::options_t options;
  options["access_id"] = "option-id";
  options["access_key"] = "option-key";
  options["endpoint"] = "option-endpoint";
  auto conn = OdpsConnectionOptionsBuilder(makeSchema({}, options)).build();
  EXPECT_EQ(conn.accessId, "option-id");
  EXPECT_EQ(conn.endpoint, "option-endpoint");
}

TEST(OdpsConnectionTest, MissingCredentialsThrows) {
  ScopedOdpsEnv env(allOdpsEnvKeys());
  reader::options_t options;
  options["endpoint"] = "ep";  // credentials absent from options and env
  EXPECT_THROW(OdpsConnectionOptionsBuilder(makeSchema({}, options)).build(),
               exception::InvalidArgumentException);
}

TEST(OdpsConnectionTest, MissingEndpointThrows) {
  ScopedOdpsEnv env(allOdpsEnvKeys());
  reader::options_t options;
  options["access_id"] = "id";
  options["access_key"] = "key";  // endpoint absent
  EXPECT_THROW(OdpsConnectionOptionsBuilder(makeSchema({}, options)).build(),
               exception::InvalidArgumentException);
}

TEST(OdpsConnectionTest, HandleUnavailableWithoutSdk) {
  // In a skeleton build (NEUG_WITH_ODPS_SDK=OFF) the glue handle stays null but
  // the connection object is still usable for option storage.
  OdpsConnectionOptions opts;
  opts.accessId = "id";
  opts.accessKey = "key";
  opts.endpoint = "http://example/endpoint";
  OdpsConnection conn(opts);
  EXPECT_EQ(conn.options().endpoint, "http://example/endpoint");
#if !defined(ODPS_SDK_ENABLE_ARROW)
  EXPECT_FALSE(conn.available());
  EXPECT_EQ(conn.glueHandle(), nullptr);
#endif
}

// ============================================================================
// maskCredential
// ============================================================================

TEST(OdpsConnectionTest, MasksCredentials) {
  EXPECT_EQ(maskCredential(""), "(not set)");
  EXPECT_EQ(maskCredential("abc"), "***");
  EXPECT_EQ(maskCredential("abcd"), "***");
  EXPECT_EQ(maskCredential("abcdefgh"), "abcd***");
}

// ============================================================================
// OdpsSchemaConverter (T105 pure type-mapping core, SDK-free)
// ============================================================================

namespace {
int code(OdpsTypeCode t) { return static_cast<int>(t); }
}  // namespace

TEST(OdpsSchemaConverterTest, MapsIntegerWidths) {
  // NeuG DataType has no 8/16-bit primitive: narrow ints collapse to INT32.
  for (auto t : {OdpsTypeCode::kTinyint, OdpsTypeCode::kSmallint,
                 OdpsTypeCode::kInteger}) {
    auto dt = OdpsSchemaConverter::convertScalarType(code(t), "c");
    ASSERT_TRUE(dt->has_primitive_type());
    EXPECT_EQ(dt->primitive_type(), ::common::PrimitiveType::DT_SIGNED_INT32);
  }
  auto bigint =
      OdpsSchemaConverter::convertScalarType(code(OdpsTypeCode::kBigint), "c");
  EXPECT_EQ(bigint->primitive_type(), ::common::PrimitiveType::DT_SIGNED_INT64);
}

TEST(OdpsSchemaConverterTest, MapsFloatDoubleBoolean) {
  EXPECT_EQ(
      OdpsSchemaConverter::convertScalarType(code(OdpsTypeCode::kFloat), "c")
          ->primitive_type(),
      ::common::PrimitiveType::DT_FLOAT);
  EXPECT_EQ(
      OdpsSchemaConverter::convertScalarType(code(OdpsTypeCode::kDouble), "c")
          ->primitive_type(),
      ::common::PrimitiveType::DT_DOUBLE);
  EXPECT_EQ(
      OdpsSchemaConverter::convertScalarType(code(OdpsTypeCode::kBoolean), "c")
          ->primitive_type(),
      ::common::PrimitiveType::DT_BOOL);
}

TEST(OdpsSchemaConverterTest, MapsStringFamilyToVarChar) {
  for (auto t :
       {OdpsTypeCode::kString, OdpsTypeCode::kVarchar, OdpsTypeCode::kChar}) {
    auto dt = OdpsSchemaConverter::convertScalarType(code(t), "c");
    ASSERT_TRUE(dt->has_string());
    EXPECT_TRUE(dt->string().has_var_char());
  }
}

TEST(OdpsSchemaConverterTest, SupportedScalarClassification) {
  for (auto t :
       {OdpsTypeCode::kTinyint, OdpsTypeCode::kSmallint, OdpsTypeCode::kInteger,
        OdpsTypeCode::kBigint, OdpsTypeCode::kFloat, OdpsTypeCode::kDouble,
        OdpsTypeCode::kBoolean, OdpsTypeCode::kString, OdpsTypeCode::kVarchar,
        OdpsTypeCode::kChar}) {
    EXPECT_TRUE(OdpsSchemaConverter::isSupportedScalar(code(t)));
  }
  for (auto t :
       {OdpsTypeCode::kDecimal, OdpsTypeCode::kDate, OdpsTypeCode::kDatetime,
        OdpsTypeCode::kTimestamp, OdpsTypeCode::kTimestampNtz,
        OdpsTypeCode::kBinary, OdpsTypeCode::kArray, OdpsTypeCode::kMap,
        OdpsTypeCode::kStruct, OdpsTypeCode::kJson,
        OdpsTypeCode::kIntervalYearMonth, OdpsTypeCode::kIntervalDayTime,
        OdpsTypeCode::kUnknown}) {
    EXPECT_FALSE(OdpsSchemaConverter::isSupportedScalar(code(t)));
  }
}

TEST(OdpsSchemaConverterTest, UnsupportedTypesThrow) {
  for (auto t :
       {OdpsTypeCode::kDecimal, OdpsTypeCode::kDate, OdpsTypeCode::kDatetime,
        OdpsTypeCode::kTimestamp, OdpsTypeCode::kTimestampNtz,
        OdpsTypeCode::kBinary, OdpsTypeCode::kArray, OdpsTypeCode::kMap,
        OdpsTypeCode::kStruct, OdpsTypeCode::kJson, OdpsTypeCode::kUnknown}) {
    EXPECT_THROW(OdpsSchemaConverter::convertScalarType(code(t), "col"),
                 exception::InvalidArgumentException);
  }
  // An out-of-range raw code is treated as unknown, not silently mapped.
  EXPECT_THROW(OdpsSchemaConverter::convertScalarType(999, "col"),
               exception::InvalidArgumentException);
}

TEST(OdpsSchemaConverterTest, TypeNameIsHumanReadable) {
  EXPECT_EQ(OdpsSchemaConverter::typeName(code(OdpsTypeCode::kBigint)),
            "BIGINT");
  EXPECT_EQ(OdpsSchemaConverter::typeName(code(OdpsTypeCode::kDecimal)),
            "DECIMAL");
  EXPECT_EQ(OdpsSchemaConverter::typeName(code(OdpsTypeCode::kInteger)), "INT");
  EXPECT_NE(OdpsSchemaConverter::typeName(999).find("UNKNOWN"),
            std::string::npos);
}

TEST(OdpsSchemaConverterTest, ConvertColumnsBuildsOrderedTableSchema) {
  std::vector<OdpsColumnDesc> cols = {
      {"id", code(OdpsTypeCode::kBigint), false},
      {"name", code(OdpsTypeCode::kString), true},
      {"score", code(OdpsTypeCode::kDouble), true},
  };
  auto entry = OdpsSchemaConverter::convertColumns(cols);
  ASSERT_NE(entry, nullptr);
  EXPECT_EQ(entry->type(), reader::EntrySchemaType::TABLE);
  ASSERT_EQ(entry->columnNames.size(), 3u);
  ASSERT_EQ(entry->columnTypes.size(), 3u);
  EXPECT_EQ(entry->columnNames[0], "id");
  EXPECT_EQ(entry->columnNames[1], "name");
  EXPECT_EQ(entry->columnNames[2], "score");
  EXPECT_EQ(entry->columnTypes[0]->primitive_type(),
            ::common::PrimitiveType::DT_SIGNED_INT64);
  EXPECT_TRUE(entry->columnTypes[1]->has_string());
  EXPECT_EQ(entry->columnTypes[2]->primitive_type(),
            ::common::PrimitiveType::DT_DOUBLE);
}

TEST(OdpsSchemaConverterTest, ConvertColumnsRejectsEmpty) {
  EXPECT_THROW(OdpsSchemaConverter::convertColumns({}),
               exception::InvalidArgumentException);
}

TEST(OdpsSchemaConverterTest, ConvertColumnsRejectsEmptyName) {
  std::vector<OdpsColumnDesc> cols = {{"", code(OdpsTypeCode::kBigint), false}};
  EXPECT_THROW(OdpsSchemaConverter::convertColumns(cols),
               exception::InvalidArgumentException);
}

TEST(OdpsSchemaConverterTest, ConvertColumnsRejectsDuplicateName) {
  std::vector<OdpsColumnDesc> cols = {
      {"a", code(OdpsTypeCode::kBigint), false},
      {"a", code(OdpsTypeCode::kString), true},
  };
  EXPECT_THROW(OdpsSchemaConverter::convertColumns(cols),
               exception::InvalidArgumentException);
}

TEST(OdpsSchemaConverterTest, ConvertColumnsPropagatesUnsupportedType) {
  std::vector<OdpsColumnDesc> cols = {
      {"id", code(OdpsTypeCode::kBigint), false},
      {"amount", code(OdpsTypeCode::kDecimal), true},
  };
  EXPECT_THROW(OdpsSchemaConverter::convertColumns(cols),
               exception::InvalidArgumentException);
}

#if !defined(ODPS_SDK_ENABLE_ARROW)
TEST(OdpsSchemaConverterTest, SniffWithoutSdkThrowsClearError) {
  OdpsConnectionOptions opts;
  opts.accessId = "id";
  opts.accessKey = "key";
  opts.endpoint = "http://example/endpoint";
  opts.project = "proj";
  OdpsConnection conn(opts);
  OdpsSourceDesc source;
  source.project = "proj";
  source.table = "tbl";
  EXPECT_THROW(OdpsSchemaConverter::sniffTableSchema(conn, source),
               exception::InvalidArgumentException);
}
#endif

// ============================================================================
// OdpsError attribution (T108, SDK-free): glue "[code] msg" -> actionable cause
// ============================================================================

TEST(OdpsErrorTest, ExtractsBracketedCode) {
  EXPECT_EQ("AccessDenied", OdpsError::extractCode("[AccessDenied] no perms"));
  EXPECT_EQ("ODPS-0130131",
            OdpsError::extractCode("[ODPS-0130131] Table not found"));
  // No bracketed prefix (a bare std::exception::what()) or unbalanced -> empty.
  EXPECT_EQ("", OdpsError::extractCode("connection reset by peer"));
  EXPECT_EQ("", OdpsError::extractCode("[unclosed"));
}

TEST(OdpsErrorTest, ClassifiesAuthenticationCodes) {
  for (const char* msg :
       {"[AccessDenied] denied", "[Unauthorized] x",
        "[SignatureDoesNotMatch] bad signature",
        "[InvalidAccessKeyId.NotFound] no key", "[TokenExpired] token expired",
        "[ODPS-0410051] invalid credentials"}) {
    EXPECT_EQ(OdpsErrorCategory::kAuthentication, OdpsError::classify(msg))
        << msg;
  }
}

TEST(OdpsErrorTest, ClassifiesNotFoundCodes) {
  for (const char* msg :
       {"[NoSuchObject] gone", "[NoSuchTable] t", "[TableNotFound] t",
        "[NoSuchPartition] pt", "[ODPS-0130131] Table not found"}) {
    EXPECT_EQ(OdpsErrorCategory::kNotFound, OdpsError::classify(msg)) << msg;
  }
}

TEST(OdpsErrorTest, ClassifiesNetworkCodes) {
  for (const char* msg : {"[ConnectionError] refused", "could not resolve host",
                          "[NetworkUnreachable] no route to host",
                          "[ConnectTimeout] connect timed out"}) {
    EXPECT_EQ(OdpsErrorCategory::kNetwork, OdpsError::classify(msg)) << msg;
  }
}

TEST(OdpsErrorTest, ClassifiesTimeoutQuotaAndBadRequest) {
  EXPECT_EQ(OdpsErrorCategory::kSessionTimeout,
            OdpsError::classify("[RequestTimeout] timed out"));
  EXPECT_EQ(OdpsErrorCategory::kSessionTimeout,
            OdpsError::classify("[SessionExpired] reload the session"));
  EXPECT_EQ(OdpsErrorCategory::kQuota,
            OdpsError::classify("[QuotaExceeded] over quota"));
  EXPECT_EQ(OdpsErrorCategory::kQuota,
            OdpsError::classify("[Throttling] slow down"));
  EXPECT_EQ(OdpsErrorCategory::kBadRequest,
            OdpsError::classify("[InvalidParameter] bad partition spec"));
}

TEST(OdpsErrorTest, UnknownCodeFallsBackToUnknown) {
  EXPECT_EQ(OdpsErrorCategory::kUnknown,
            OdpsError::classify("[SomethingWeird] huh"));
  EXPECT_STREQ("Unknown", OdpsError::categoryName(OdpsErrorCategory::kUnknown));
  EXPECT_STREQ("Authentication",
               OdpsError::categoryName(OdpsErrorCategory::kAuthentication));
}

TEST(OdpsErrorTest, AttributeKeepsOperationHintAndOriginalText) {
  const std::string msg =
      OdpsError::attribute("ODPS_SCAN: failed to read schema of p.s.t",
                           "[AccessDenied] you have no permission");
  EXPECT_NE(msg.find("failed to read schema of p.s.t"), std::string::npos);
  EXPECT_NE(msg.find("authentication or authorization failed"),
            std::string::npos);
  EXPECT_NE(msg.find("[AccessDenied] you have no permission"),
            std::string::npos);
}

TEST(OdpsErrorTest, RedactSecretsMasksLongCredentialsOnly) {
  const std::string secret = "SUPERSECRETACCESSKEY123";
  const std::string redacted = OdpsError::redactSecrets(
      "signature computed from " + secret + " does not match", {secret});
  EXPECT_EQ(redacted.find(secret), std::string::npos);
  EXPECT_NE(redacted.find(maskCredential(secret)), std::string::npos);
  // Short values are left untouched so ordinary words are never masked.
  EXPECT_EQ("id", OdpsError::redactSecrets("id", {"id"}));
}

TEST(OdpsErrorTest, ThrowAttributedMapsCategoryToExceptionType) {
  EXPECT_THROW(OdpsError::throwAttributed("op", "[AccessDenied] denied"),
               exception::PermissionDeniedException);
  EXPECT_THROW(OdpsError::throwAttributed("op", "[NoSuchTable] gone"),
               exception::NotFoundException);
  EXPECT_THROW(OdpsError::throwAttributed("op", "[ConnectionError] refused"),
               exception::ConnectionException);
  EXPECT_THROW(OdpsError::throwAttributed("op", "[InvalidParameter] bad"),
               exception::InvalidArgumentException);
  // Timeout / quota / unknown all surface as IO errors.
  EXPECT_THROW(OdpsError::throwAttributed("op", "[RequestTimeout] slow"),
               exception::IOException);
  EXPECT_THROW(OdpsError::throwAttributed("op", "[Weird] huh"),
               exception::IOException);
}

TEST(OdpsErrorTest, ThrowAttributedRedactsSecretsBeforeThrowing) {
  const std::string secret = "SUPERSECRETACCESSKEY123";
  try {
    OdpsError::throwAttributed("op", "[SignatureDoesNotMatch] " + secret,
                               {secret});
    FAIL() << "throwAttributed should always throw";
  } catch (const exception::Exception& e) {
    EXPECT_EQ(std::string(e.what()).find(secret), std::string::npos)
        << e.what();
  }
}

// ============================================================================
// Arrow C Data Interface bridge (T106, SDK-free): recordBatchToDataChunk
// ============================================================================

namespace {

// Zero-initialized scalar-column schema. `format`/`name` must outlive the
// returned struct; tests pass string literals (static storage duration).
OdpsArrowSchema makeScalarSchema(const char* format, const char* name) {
  OdpsArrowSchema s{};
  s.format = format;
  s.name = name;
  s.flags = ODPS_ARROW_FLAG_NULLABLE;
  return s;
}

// Zero-initialized scalar-column array over `buffers`.
OdpsArrowArray makeScalarArray(int64_t length, int64_t nullCount,
                               std::vector<const void*>& buffers) {
  OdpsArrowArray a{};
  a.length = length;
  a.null_count = nullCount;
  a.n_buffers = static_cast<int64_t>(buffers.size());
  a.buffers = buffers.data();
  return a;
}

// Packs a per-row validity mask into Arrow's LSB0 bitmap and reports the null
// count. An empty mask means "all valid"; callers then export a null validity
// buffer, which the bridge treats as having no nulls.
std::vector<uint8_t> packValidity(const std::vector<bool>& valid,
                                  int64_t& nullCount) {
  std::vector<uint8_t> bits((valid.size() + 7) / 8, 0);
  nullCount = 0;
  for (size_t i = 0; i < valid.size(); ++i) {
    if (valid[i]) {
      bits[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
    } else {
      ++nullCount;
    }
  }
  return bits;
}

// Hand-builds an Arrow C Data Interface record batch: a top-level "+s" struct
// of scalar columns. Owns every backing buffer plus the schema/array structs
// and their child pointer tables, so the exported batch stays valid until the
// builder dies. The bridge deep-copies values and never invokes `release`, so
// release callbacks are intentionally left null. Example:
//
//   RecordBatchBuilder batch(3);
//   batch.addInt64("id", {10, 20, 30}).addString("name", {"a", "b", "c"});
//   auto chunk = recordBatchToDataChunk(batch.schema(), batch.array());
class RecordBatchBuilder {
 public:
  explicit RecordBatchBuilder(int64_t numRows) : numRows_(numRows) {}

  RecordBatchBuilder& addInt64(const char* name, std::vector<int64_t> values,
                               std::vector<bool> valid = {}) {
    return addFixed("l", name, std::move(values), std::move(valid),
                    &Column::i64);
  }
  RecordBatchBuilder& addInt32(const char* name, std::vector<int32_t> values,
                               std::vector<bool> valid = {}) {
    return addFixed("i", name, std::move(values), std::move(valid),
                    &Column::i32);
  }
  RecordBatchBuilder& addDouble(const char* name, std::vector<double> values,
                                std::vector<bool> valid = {}) {
    return addFixed("g", name, std::move(values), std::move(valid),
                    &Column::f64);
  }
  RecordBatchBuilder& addFloat(const char* name, std::vector<float> values,
                               std::vector<bool> valid = {}) {
    return addFixed("f", name, std::move(values), std::move(valid),
                    &Column::f32);
  }
  RecordBatchBuilder& addDate32(const char* name, std::vector<int32_t> days,
                                std::vector<bool> valid = {}) {
    return addFixed("tdD", name, std::move(days), std::move(valid),
                    &Column::i32);
  }
  RecordBatchBuilder& addTimestampMillis(const char* name,
                                         std::vector<int64_t> millis,
                                         std::vector<bool> valid = {}) {
    return addFixed("tsm:", name, std::move(millis), std::move(valid),
                    &Column::i64);
  }

  // Bit-packed boolean column (Arrow format "b").
  RecordBatchBuilder& addBool(const char* name, const std::vector<bool>& values,
                              std::vector<bool> valid = {}) {
    auto col = std::make_unique<Column>();
    col->boolBits.assign((values.size() + 7) / 8, 0);
    for (size_t i = 0; i < values.size(); ++i) {
      if (values[i]) {
        col->boolBits[i >> 3] |= static_cast<uint8_t>(1u << (i & 7));
      }
    }
    const void* data = col->boolBits.data();
    finalize("b", name, valid, {data}, std::move(col));
    return *this;
  }

  // UTF-8 string column (Arrow format "u", 32-bit offsets).
  RecordBatchBuilder& addString(const char* name,
                                const std::vector<std::string>& values,
                                std::vector<bool> valid = {}) {
    auto col = std::make_unique<Column>();
    col->strOffsets.reserve(values.size() + 1);
    col->strOffsets.push_back(0);
    for (const auto& s : values) {
      col->strOffsets.push_back(col->strOffsets.back() +
                                static_cast<int32_t>(s.size()));
    }
    for (const auto& s : values) {
      col->strChars.insert(col->strChars.end(), s.begin(), s.end());
    }
    const void* offsets = col->strOffsets.data();
    const void* chars = col->strChars.data();
    finalize("u", name, valid, {offsets, chars}, std::move(col));
    return *this;
  }

  const OdpsArrowSchema& schema() {
    ensureRoot();
    return rootSchema_;
  }
  const OdpsArrowArray& array() {
    ensureRoot();
    return rootArray_;
  }

 private:
  // Owns the backing memory for one scalar column. Only the storage relevant
  // to the column's Arrow type is populated; the rest stay empty.
  struct Column {
    OdpsArrowSchema schema{};
    OdpsArrowArray array{};
    std::vector<const void*> buffers;
    std::vector<uint8_t> validity;
    std::vector<int64_t> i64;
    std::vector<int32_t> i32;
    std::vector<double> f64;
    std::vector<float> f32;
    std::vector<uint8_t> boolBits;
    std::vector<int32_t> strOffsets;
    std::vector<char> strChars;
  };

  template <typename T>
  RecordBatchBuilder& addFixed(const char* format, const char* name,
                               std::vector<T> values, std::vector<bool> valid,
                               std::vector<T> Column::*storage) {
    auto col = std::make_unique<Column>();
    (col.get()->*storage) = std::move(values);
    const void* data = (col.get()->*storage).data();
    finalize(format, name, valid, {data}, std::move(col));
    return *this;
  }

  // Sets the column schema, packs the validity bitmap (when `valid` is
  // non-empty), prepends the validity buffer to `dataBuffers`, and records the
  // child array.
  void finalize(const char* format, const char* name,
                const std::vector<bool>& valid,
                std::vector<const void*> dataBuffers,
                std::unique_ptr<Column> col) {
    col->schema = makeScalarSchema(format, name);
    int64_t nullCount = 0;
    const bool nullable = !valid.empty();
    if (nullable) {
      col->validity = packValidity(valid, nullCount);
    }
    std::vector<const void*> buffers;
    buffers.reserve(dataBuffers.size() + 1);
    buffers.push_back(nullable ? static_cast<const void*>(col->validity.data())
                               : nullptr);
    for (const void* b : dataBuffers) {
      buffers.push_back(b);
    }
    col->buffers = std::move(buffers);
    col->array = makeScalarArray(numRows_, nullCount, col->buffers);
    columns_.push_back(std::move(col));
  }

  void ensureRoot() {
    if (rootBuilt_) {
      return;
    }
    childSchemas_.reserve(columns_.size());
    childArrays_.reserve(columns_.size());
    for (const auto& col : columns_) {
      childSchemas_.push_back(&col->schema);
      childArrays_.push_back(&col->array);
    }
    rootSchema_ = OdpsArrowSchema{};
    rootSchema_.format = "+s";
    rootSchema_.n_children = static_cast<int64_t>(columns_.size());
    rootSchema_.children =
        childSchemas_.empty() ? nullptr : childSchemas_.data();

    rootBuffers_.assign(1, nullptr);
    rootArray_ = OdpsArrowArray{};
    rootArray_.length = numRows_;
    rootArray_.null_count = 0;
    rootArray_.offset = 0;
    rootArray_.n_buffers = 1;
    rootArray_.n_children = static_cast<int64_t>(columns_.size());
    rootArray_.buffers = rootBuffers_.data();
    rootArray_.children = childArrays_.empty() ? nullptr : childArrays_.data();
    rootBuilt_ = true;
  }

  int64_t numRows_;
  bool rootBuilt_ = false;
  std::vector<std::unique_ptr<Column>> columns_;
  std::vector<OdpsArrowSchema*> childSchemas_;
  std::vector<OdpsArrowArray*> childArrays_;
  std::vector<const void*> rootBuffers_;
  OdpsArrowSchema rootSchema_{};
  OdpsArrowArray rootArray_{};
};

TEST(OdpsArrowBridgeTest, ConvertsScalarColumnsIntoDataChunk) {
  RecordBatchBuilder batch(3);
  batch.addInt64("id", {10, 20, 30})
      .addInt32("qty", {1, 2, 3})
      .addDouble("score", {1.5, 2.5, 3.5})
      .addFloat("ratio", {0.25f, 0.5f, 0.75f})
      .addBool("flag", {true, false, true})
      .addString("name", {"alice", "bob", "carol"});

  auto chunk = recordBatchToDataChunk(batch.schema(), batch.array());
  ASSERT_NE(chunk, nullptr);
  EXPECT_EQ(chunk->col_num(), 6u);
  EXPECT_EQ(chunk->row_num(), 3u);

  auto id = chunk->get(0);
  ASSERT_NE(id, nullptr);
  EXPECT_EQ(id->size(), 3u);
  EXPECT_FALSE(id->is_optional());
  EXPECT_EQ(id->get_elem(0).GetValue<int64_t>(), 10);
  EXPECT_EQ(id->get_elem(1).GetValue<int64_t>(), 20);
  EXPECT_EQ(id->get_elem(2).GetValue<int64_t>(), 30);

  auto qty = chunk->get(1);
  EXPECT_EQ(qty->get_elem(0).GetValue<int32_t>(), 1);
  EXPECT_EQ(qty->get_elem(2).GetValue<int32_t>(), 3);

  auto score = chunk->get(2);
  EXPECT_DOUBLE_EQ(score->get_elem(1).GetValue<double>(), 2.5);

  auto ratio = chunk->get(3);
  EXPECT_FLOAT_EQ(ratio->get_elem(0).GetValue<float>(), 0.25f);

  auto flag = chunk->get(4);
  EXPECT_TRUE(flag->get_elem(0).GetValue<bool>());
  EXPECT_FALSE(flag->get_elem(1).GetValue<bool>());
  EXPECT_TRUE(flag->get_elem(2).GetValue<bool>());

  auto name = chunk->get(5);
  EXPECT_EQ(StringValue::Get(name->get_elem(0)), "alice");
  EXPECT_EQ(StringValue::Get(name->get_elem(1)), "bob");
  EXPECT_EQ(StringValue::Get(name->get_elem(2)), "carol");
}

TEST(OdpsArrowBridgeTest, PreservesNullsAcrossColumnTypes) {
  RecordBatchBuilder batch(3);
  batch.addInt64("id", {10, 0, 30}, {true, false, true})
      .addString("name", {"alice", "", "carol"}, {true, false, true})
      .addDouble("score", {1.5, 2.5, 0.0}, {true, true, false});

  auto chunk = recordBatchToDataChunk(batch.schema(), batch.array());
  ASSERT_EQ(chunk->col_num(), 3u);

  auto id = chunk->get(0);
  EXPECT_TRUE(id->is_optional());
  EXPECT_TRUE(id->has_value(0));
  EXPECT_FALSE(id->has_value(1));
  EXPECT_TRUE(id->has_value(2));
  EXPECT_EQ(id->get_elem(0).GetValue<int64_t>(), 10);
  EXPECT_TRUE(id->get_elem(1).IsNull());
  EXPECT_EQ(id->get_elem(2).GetValue<int64_t>(), 30);

  auto name = chunk->get(1);
  EXPECT_EQ(StringValue::Get(name->get_elem(0)), "alice");
  EXPECT_TRUE(name->get_elem(1).IsNull());
  EXPECT_EQ(StringValue::Get(name->get_elem(2)), "carol");

  auto score = chunk->get(2);
  EXPECT_TRUE(score->has_value(0));
  EXPECT_TRUE(score->has_value(1));
  EXPECT_FALSE(score->has_value(2));
  EXPECT_DOUBLE_EQ(score->get_elem(1).GetValue<double>(), 2.5);
  EXPECT_TRUE(score->get_elem(2).IsNull());
}

TEST(OdpsArrowBridgeTest, ConvertsEmptyStringsAndUnicode) {
  RecordBatchBuilder batch(3);
  batch.addString("s", {"", "\xe4\xb8\xad\xe6\x96\x87", "tail"});

  auto chunk = recordBatchToDataChunk(batch.schema(), batch.array());
  auto col = chunk->get(0);
  ASSERT_EQ(col->size(), 3u);
  EXPECT_EQ(StringValue::Get(col->get_elem(0)), "");
  // UTF-8 bytes are copied verbatim (no transcoding):
  // "\xe4\xb8\xad\xe6\x96\x87".
  EXPECT_EQ(StringValue::Get(col->get_elem(1)),
            std::string("\xe4\xb8\xad\xe6\x96\x87"));
  EXPECT_EQ(StringValue::Get(col->get_elem(2)), "tail");
}

TEST(OdpsArrowBridgeTest, ConvertsDateAndTimestampColumns) {
  RecordBatchBuilder batch(2);
  batch.addDate32("d", {19000, 19001})
      .addTimestampMillis("ts", {1234567890000LL, 1234567890001LL});

  auto chunk = recordBatchToDataChunk(batch.schema(), batch.array());
  ASSERT_EQ(chunk->col_num(), 2u);
  EXPECT_EQ(chunk->get(0)->get_elem(0).GetValue<date_t>(), Date(19000));
  EXPECT_EQ(chunk->get(0)->get_elem(1).GetValue<date_t>(), Date(19001));
  EXPECT_EQ(chunk->get(1)->get_elem(0).GetValue<timestamp_ms_t>().milli_second,
            1234567890000LL);
  EXPECT_EQ(chunk->get(1)->get_elem(1).GetValue<timestamp_ms_t>().milli_second,
            1234567890001LL);
}

TEST(OdpsArrowBridgeTest, EmptyBatchYieldsEmptyColumn) {
  RecordBatchBuilder batch(0);
  batch.addInt64("id", {});

  auto chunk = recordBatchToDataChunk(batch.schema(), batch.array());
  ASSERT_NE(chunk, nullptr);
  EXPECT_EQ(chunk->col_num(), 1u);
  EXPECT_EQ(chunk->row_num(), 0u);
  EXPECT_EQ(chunk->get(0)->size(), 0u);
}

TEST(OdpsArrowBridgeTest, EmptyStructYieldsNoColumns) {
  RecordBatchBuilder batch(0);
  auto chunk = recordBatchToDataChunk(batch.schema(), batch.array());
  ASSERT_NE(chunk, nullptr);
  EXPECT_EQ(chunk->col_num(), 0u);
  EXPECT_EQ(chunk->row_num(), 0u);
}

// A minimal, fully-valid single int64-column batch whose fields are exposed so
// each error test can corrupt exactly one and assert the bridge rejects it.
struct SingleInt64Batch {
  int64_t data[2] = {7, 9};
  const void* childBuffers[2] = {nullptr, data};
  OdpsArrowSchema childSchema{};
  OdpsArrowArray childArray{};
  OdpsArrowSchema* schemaChildren[1] = {&childSchema};
  OdpsArrowArray* arrayChildren[1] = {&childArray};
  const void* rootBuffers[1] = {nullptr};
  OdpsArrowSchema dummyDict{};
  OdpsArrowSchema schema{};
  OdpsArrowArray array{};

  SingleInt64Batch() {
    childSchema.format = "l";
    childSchema.name = "n";
    childSchema.flags = ODPS_ARROW_FLAG_NULLABLE;
    childArray.length = 2;
    childArray.n_buffers = 2;
    childArray.buffers = childBuffers;

    schema.format = "+s";
    schema.n_children = 1;
    schema.children = schemaChildren;
    array.length = 2;
    array.n_buffers = 1;
    array.buffers = rootBuffers;
    array.n_children = 1;
    array.children = arrayChildren;
  }
};

TEST(OdpsArrowBridgeTest, RejectsNonStructRoot) {
  SingleInt64Batch b;
  b.schema.format = "l";
  EXPECT_THROW(recordBatchToDataChunk(b.schema, b.array),
               exception::InvalidArgumentException);
}

TEST(OdpsArrowBridgeTest, RejectsNullRootFormat) {
  SingleInt64Batch b;
  b.schema.format = nullptr;
  EXPECT_THROW(recordBatchToDataChunk(b.schema, b.array),
               exception::InvalidArgumentException);
}

TEST(OdpsArrowBridgeTest, RejectsChildCountMismatch) {
  SingleInt64Batch b;
  b.array.n_children = 2;  // schema declares 1
  EXPECT_THROW(recordBatchToDataChunk(b.schema, b.array),
               exception::InvalidArgumentException);
}

TEST(OdpsArrowBridgeTest, RejectsNullableRootStruct) {
  SingleInt64Batch b;
  b.array.null_count = 1;
  EXPECT_THROW(recordBatchToDataChunk(b.schema, b.array),
               exception::InvalidArgumentException);
}

TEST(OdpsArrowBridgeTest, RejectsUnsupportedChildFormat) {
  SingleInt64Batch b;
  b.childSchema.format = "d:38,10";  // decimal128 -> module 4 (T401)
  EXPECT_THROW(recordBatchToDataChunk(b.schema, b.array),
               exception::InvalidArgumentException);
}

TEST(OdpsArrowBridgeTest, RejectsDictionaryColumn) {
  SingleInt64Batch b;
  b.childSchema.dictionary = &b.dummyDict;
  EXPECT_THROW(recordBatchToDataChunk(b.schema, b.array),
               exception::InvalidArgumentException);
}

TEST(OdpsArrowBridgeTest, RejectsChildLengthMismatch) {
  SingleInt64Batch b;
  b.childArray.length = 1;  // root struct has length 2
  EXPECT_THROW(recordBatchToDataChunk(b.schema, b.array),
               exception::InvalidArgumentException);
}

TEST(OdpsArrowBridgeTest, RejectsNullChild) {
  SingleInt64Batch b;
  b.arrayChildren[0] = nullptr;
  EXPECT_THROW(recordBatchToDataChunk(b.schema, b.array),
               exception::InvalidArgumentException);
}

TEST(OdpsArrowBridgeTest, RejectsNullsWithoutValidityBuffer) {
  SingleInt64Batch b;
  b.childArray.null_count = 1;  // childBuffers[0] stays null
  EXPECT_THROW(recordBatchToDataChunk(b.schema, b.array),
               exception::InvalidArgumentException);
}

TEST(OdpsArrowBridgeTest, RejectsWrongBufferCount) {
  SingleInt64Batch b;
  b.childArray.n_buffers = 3;  // int64 column expects exactly 2
  EXPECT_THROW(recordBatchToDataChunk(b.schema, b.array),
               exception::InvalidArgumentException);
}

}  // namespace

namespace {

// --- End-to-end LOAD FROM routing (SDK-OFF) ---------------------------------
// The data plane itself needs the x86_64-only ODPS SDK plus live credentials,
// so it cannot run here. What CAN be verified without the SDK is the query
// routing: that an `odps://` source resolves to this extension's ODPS_SCAN (via
// the {SCHEME}_SCAN catalog lookup added to the binder) instead of failing to
// parse the address as a file extension. With routing correct the scan reaches
// sniffFunc, which raises the SDK-required error asserted below.

std::filesystem::path GetExecutablePath() {
#if defined(__APPLE__)
  uint32_t size = 0;
  _NSGetExecutablePath(nullptr, &size);
  std::string buffer(size, '\0');
  if (_NSGetExecutablePath(buffer.data(), &size) != 0) {
    return {};
  }
  return std::filesystem::canonical(buffer.c_str());
#else
  return std::filesystem::read_symlink("/proc/self/exe");
#endif
}

// Walks up from the test binary to the build root holding the loadable
// libodps.neug_extension, mirroring the fts extension load-smoke test.
std::string FindOdpsBuildRoot() {
  auto directory = GetExecutablePath().parent_path();
  const auto extension_path =
      std::filesystem::path("extension/odps/libodps.neug_extension");
  for (int i = 0; i < 8; ++i) {
    if (std::filesystem::exists(directory / extension_path)) {
      return directory.string();
    }
    if (directory == directory.parent_path()) {
      break;
    }
    directory = directory.parent_path();
  }
  return "";
}

class TemporaryDatabaseDirectory {
 public:
  TemporaryDatabaseDirectory() {
    const auto suffix =
        std::chrono::steady_clock::now().time_since_epoch().count();
    path_ = std::filesystem::temp_directory_path() /
            ("neug_odps_routing_" + std::to_string(suffix));
  }
  ~TemporaryDatabaseDirectory() { std::filesystem::remove_all(path_); }
  const std::filesystem::path& path() const { return path_; }

 private:
  std::filesystem::path path_;
};

TEST(OdpsLoadFromRoutingTest, OdpsSchemeReachesOdpsScanWithoutSdk) {
  const auto build_root = FindOdpsBuildRoot();
  ASSERT_FALSE(build_root.empty())
      << "could not locate libodps.neug_extension above the test binary";
  ASSERT_EQ(setenv("NEUG_EXTENSION_HOME_PYENV", build_root.c_str(), 1), 0);

  // Connection options are resolved before the scan reaches sniffFunc, so
  // supply dummy credentials/endpoint to clear that gate deterministically; the
  // SDK-OFF sniff short-circuits to its error before any network call.
  // ScopedOdpsEnv makes this independent of test order and restores the
  // environment after.
  ScopedOdpsEnv env(allOdpsEnvKeys());
  env.set(OdpsConnectionKeys::kEnvAccessKeyId, "routing-dummy-id");
  env.set(OdpsConnectionKeys::kEnvAccessKeySecret, "routing-dummy-secret");
  env.set(OdpsConnectionKeys::kEnvEndpoint, "routing-dummy-endpoint");

  TemporaryDatabaseDirectory database_directory;
  neug::NeugDB database;
  ASSERT_TRUE(database.Open(database_directory.path()));
  auto connection = database.Connect();
  ASSERT_NE(connection, nullptr);

  auto load = connection->Query("LOAD odps;");
  ASSERT_TRUE(load.has_value()) << load.error().ToString();

  // No SDK build: the scan cannot fetch data, but it MUST route to ODPS_SCAN
  // and surface the SDK-required error from sniffFunc. A regression in scheme
  // detection would instead fail earlier with a "{...}_SCAN does not exist"
  // catalog error naming a mis-parsed fragment of the address (e.g. MY_TABLE).
  auto result = connection->Query(
      "LOAD FROM \"odps://my_project.default.my_table\" RETURN *;");
  ASSERT_FALSE(result.has_value());
  const auto message = result.error().ToString();
  EXPECT_NE(message.find("ODPS_SCAN"), std::string::npos) << message;
  EXPECT_NE(message.find("requires the ODPS SDK build"), std::string::npos)
      << message;
}

}  // namespace

}  // namespace
}  // namespace odps
}  // namespace extension
}  // namespace neug
