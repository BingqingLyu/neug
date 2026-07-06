# Feature Specification: Iceberg Catalog Integration

**Feature Branch**: `004-iceberg-catalog`  
**Created**: 2026-06-04  
**Status**: Draft  
**Priority**: P4 (Future)  
**Depends On**: 004-iceberg-extension (P1-P3 已完成)

---

## Background

NeuG 当前的 Iceberg 支持接入的是 **Metadata 层**——用户需要提供表的物理存储路径（如 `oss://bucket/warehouse/person`）来访问表。这种方式零依赖、简单直接，但在企业级部署中存在以下不便：

- 用户需要知道每张表的物理路径
- 无法通过逻辑名称发现和引用表
- 当表的物理位置迁移时，所有查询需要更新路径
- 无法利用 catalog 提供的统一权限管理

Iceberg Catalog 是表注册中心，管理 namespace → table name → metadata-location 的映射关系。接入 Catalog 层后，用户可以通过逻辑名称引用表。

---

## Iceberg 架构层次与接入点

```
┌─────────────────────────────────────────────────┐
│  Catalog 层（REST / Hive Metastore / Glue）     │  ← 本 spec 目标
│  namespace.table_name → metadata-location 映射   │
├─────────────────────────────────────────────────┤
│  Metadata 层（metadata.json）                    │  ← 已接入（P1）
│  schema、snapshot、partition-spec                │
├─────────────────────────────────────────────────┤
│  Manifest 层                                     │  ← 已接入（P1）
├─────────────────────────────────────────────────┤
│  Data 层（Parquet）                              │  ← 已接入（P1）
└─────────────────────────────────────────────────┘
```

Catalog 的唯一职责是：**将逻辑表名解析为 metadata.json 的物理路径**。解析完成后，后续流程完全复用已有的 Metadata → Manifest → Data 链路。

---

## Supported Catalog Types

### Phase 1: REST Catalog（推荐首先支持）

REST Catalog 是 Iceberg 社区推荐的标准化 Catalog 协议，具有：
- 语言无关（HTTP + JSON）
- 无状态部署
- 已被 Tabular、Snowflake、Databricks 等采用

### Phase 2: Hive Metastore（可选）

需要 Thrift 客户端，依赖较重，延后考虑。

### Phase 3: AWS Glue / Alibaba DLF（可选）

云厂商私有 API，按需支持。

---

## Functional Requirements

### FR-C01: REST Catalog 表解析

用户通过 catalog 参数指定连接信息，使用逻辑表名访问数据：

```cypher
LOAD FROM "db.person" (
    FILE_FORMAT='iceberg',
    CATALOG_TYPE='rest',
    CATALOG_URI='https://catalog.example.com',
    WAREHOUSE='s3://my-bucket/warehouse',
    CREDENTIALS_KIND='Default',
    ENDPOINT_OVERRIDE='oss-cn-beijing.aliyuncs.com'
)
RETURN *
```

扩展内部流程：
1. 解析 `CATALOG_TYPE='rest'` → 使用 REST Catalog 客户端
2. 调用 REST API: `GET /v1/namespaces/db/tables/person` 获取 `metadata-location`
3. 用返回的 `metadata-location` 走现有的 metadata 解析链路

### FR-C02: Namespace 列表查询

```cypher
-- 列出 catalog 中所有 namespace
CALL iceberg.list_namespaces(
    catalog_uri := 'https://catalog.example.com',
    warehouse := 's3://my-bucket/warehouse'
)

-- 列出某 namespace 下的所有表
CALL iceberg.list_tables(
    catalog_uri := 'https://catalog.example.com',
    warehouse := 's3://my-bucket/warehouse',
    namespace := 'db'
)
```

### FR-C03: Catalog 认证

| 认证方式 | 参数 | 说明 |
|----------|------|------|
| Bearer Token | `TOKEN='xxx'` | 静态 token |
| OAuth2 | `OAUTH2_SERVER_URI`, `CREDENTIAL`, `SCOPE` | OAuth2 client credentials flow |
| 无认证 | 默认 | 开发/测试环境 |

### FR-C04: Catalog 配置持久化（可选）

为避免每次查询都重复指定 catalog 参数，支持会话级别配置：

```cypher
-- 配置默认 catalog
SET iceberg.catalog_uri = 'https://catalog.example.com';
SET iceberg.warehouse = 's3://my-bucket/warehouse';
SET iceberg.catalog_type = 'rest';

-- 之后可以简写
LOAD FROM "db.person" (FILE_FORMAT='iceberg')
RETURN *
```

---

## REST Catalog API 对接

基于 [Iceberg REST Catalog OpenAPI Spec](https://github.com/apache/iceberg/blob/main/open-api/rest-catalog-open-api.yaml)：

| 操作 | API | 用途 |
|------|-----|------|
| 获取配置 | `GET /v1/config` | 获取 catalog 默认配置 |
| 列出 namespace | `GET /v1/namespaces` | 表发现 |
| 列出表 | `GET /v1/namespaces/{ns}/tables` | 表发现 |
| 加载表 | `GET /v1/namespaces/{ns}/tables/{table}` | 获取 metadata-location |
| Token 刷新 | `POST /v1/oauth/tokens` | OAuth2 认证 |

### 关键响应字段

`GET /v1/namespaces/{ns}/tables/{table}` 响应中：

```json
{
  "metadata-location": "s3://bucket/warehouse/db/person/metadata/v3.metadata.json",
  "metadata": { ... }
}
```

扩展只需提取 `metadata-location`，然后交给现有的 `readTableMetadata()` 流程。

---

## 内部设计

### 组件关系

```
用户查询: LOAD FROM "db.person" (CATALOG_TYPE='rest', ...)
  │
  ▼
┌─────────────────────────┐
│  IcebergCatalogResolver  │  ← 新增组件
│  判断是逻辑名还是物理路径 │
└────────────┬────────────┘
             │
    ┌────────┴────────┐
    ▼                 ▼
物理路径            逻辑名称
(已有流程)        (REST Catalog)
    │                 │
    │         ┌───────▼───────┐
    │         │ RESTCatalogClient │  ← 新增组件
    │         │ HTTP GET → metadata-location │
    │         └───────┬───────┘
    │                 │
    ▼                 ▼
┌─────────────────────────┐
│  readTableMetadata()     │  ← 现有代码，完全复用
│  → snapshot → manifest   │
│  → data files → read     │
└─────────────────────────┘
```

### 路径 vs 逻辑名判断规则

| 输入形式 | 判断为 | 示例 |
|----------|--------|------|
| 包含 `://` | 物理路径 | `s3://bucket/path`, `oss://bucket/path` |
| 以 `/` 开头 | 本地物理路径 | `/home/user/iceberg_table` |
| `namespace.table` 格式 | 逻辑名称 | `db.person`, `prod.orders` |
| 其他 | 逻辑名称 | `person`（使用默认 namespace） |

### 类设计

```cpp
// 新增：Catalog 接口
class IcebergCatalog {
 public:
  virtual ~IcebergCatalog() = default;
  // 解析逻辑表名 → metadata.json 物理路径
  virtual std::string resolveTable(
      const std::string& namespace_name,
      const std::string& table_name) = 0;
  // 列出 namespace
  virtual std::vector<std::string> listNamespaces() = 0;
  // 列出某 namespace 下的表
  virtual std::vector<std::string> listTables(
      const std::string& namespace_name) = 0;
};

// 新增：REST Catalog 实现
class RESTCatalog : public IcebergCatalog {
 public:
  RESTCatalog(const std::string& uri,
              const std::string& warehouse,
              const CatalogCredentials& credentials);
  std::string resolveTable(...) override;
  std::vector<std::string> listNamespaces() override;
  std::vector<std::string> listTables(...) override;
 private:
  HttpClient http_client_;
  std::string base_uri_;
  std::string warehouse_;
};
```

---

## 参数说明

| 参数 | 必填 | 默认值 | 说明 |
|------|------|--------|------|
| `CATALOG_TYPE` | 是 | — | catalog 类型：`'rest'` |
| `CATALOG_URI` | 是 | — | REST Catalog 服务地址 |
| `WAREHOUSE` | 否 | catalog 默认 | S3/OSS warehouse 根路径 |
| `TOKEN` | 否 | — | Bearer token |
| `OAUTH2_SERVER_URI` | 否 | — | OAuth2 token endpoint |
| `CREDENTIAL` | 否 | — | OAuth2 client_id:client_secret |
| `SCOPE` | 否 | `"catalog"` | OAuth2 scope |

> 存储层参数（`CREDENTIALS_KIND`、`ENDPOINT_OVERRIDE`）仍通过现有 httpfs 扩展处理。

---

## Acceptance Scenarios

1. **Given** REST Catalog 运行在 `https://catalog.example.com`，namespace `db` 下有表 `person`，**When** 用户执行：
   ```cypher
   LOAD FROM "db.person" (
       FILE_FORMAT='iceberg',
       CATALOG_TYPE='rest',
       CATALOG_URI='https://catalog.example.com'
   ) RETURN *
   ```
   **Then** 系统通过 REST API 获取 metadata-location，正确返回表数据。

2. **Given** catalog 中 namespace `db` 下有 3 张表，**When** 用户执行 `CALL iceberg.list_tables(..., namespace := 'db')`，**Then** 返回 3 张表的名称。

3. **Given** catalog 需要 OAuth2 认证，**When** 用户提供正确的 `OAUTH2_SERVER_URI` 和 `CREDENTIAL`，**Then** 扩展自动获取 token 并成功访问 catalog。

4. **Given** 用户指定了不存在的逻辑表名，**When** 执行查询，**Then** 返回清晰的错误：`Table 'db.xxx' not found in catalog. Available tables: [person, orders, ...]`。

---

## Dependencies

| 依赖 | 说明 |
|------|------|
| Iceberg Extension (P1-P3) | metadata 解析链路已实现 |
| httpfs Extension | 远程存储访问 |
| HTTP 客户端库 | 需要引入（如 cpp-httplib 或复用 brpc） |
| JSON 解析 | 已有 rapidjson |

---

## Test Strategy

- **Unit Tests**:
  - REST API 响应解析（正常响应、错误响应、认证失败）
  - 逻辑名 vs 物理路径判断逻辑
  - OAuth2 token 获取和刷新
- **Integration Tests**:
  - Mock REST Catalog 服务 + 端到端查询
  - 真实 REST Catalog（如 Tabular / Polaris）集成测试（CI 可选）

---

## Out of Scope

- Hive Metastore Catalog（需 Thrift，延后）
- AWS Glue Catalog（云厂商专有 API，延后）
- 表写入/创建（NeuG Iceberg 为只读）
- Catalog 级别的权限管理（由 catalog 服务自身处理）
- 多版本 REST API 兼容（初期只支持 v1）
