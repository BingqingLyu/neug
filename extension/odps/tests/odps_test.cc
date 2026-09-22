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

#include <cstdlib>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "neug/utils/exception/exception.h"

#include "odps_connection.h"
#include "odps_options.h"
#include "odps_schema_converter.h"

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
  // In a skeleton build (NEUG_WITH_ODPS_SDK=OFF) the SDK handle stays null but
  // the connection object is still usable for option storage.
  OdpsConnectionOptions opts;
  opts.accessId = "id";
  opts.accessKey = "key";
  opts.endpoint = "http://example/endpoint";
  OdpsConnection conn(opts);
  EXPECT_EQ(conn.options().endpoint, "http://example/endpoint");
#if !defined(ODPS_SDK_ENABLE_ARROW)
  EXPECT_FALSE(conn.available());
  EXPECT_EQ(conn.handle(), nullptr);
  EXPECT_FALSE(conn.clientAvailable());
  EXPECT_EQ(conn.odpsClient(), nullptr);
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

}  // namespace
}  // namespace odps
}  // namespace extension
}  // namespace neug
