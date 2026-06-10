# Implementation Plan: AP 链路临时图

**Branch**: `008-temp-graph` | **Date**: 2026-06-10 | **Spec**: [spec.md](./spec.md)
**Input**: Feature specification from `/specs/008-temp-graph/spec.md`

## Summary

在 NeuG 的 AP 链路、唯一一条 read-write Connection 上，新增会话级临时图能力。采用**共享存储 + Schema 标记**方案：临时数据与持久化数据共存于同一个 PropertyGraph 实例，通过 `VertexSchema`/`EdgeSchema` 上的 `bool temporary` 标记区分。查询路径零改动，不需要实现多图合并层。

## Technical Context

**Language/Version**: C++20，ANTLR4 Cypher 语法
**Primary Dependencies**: ANTLR4（`src/compiler/antlr4/Cypher.g4`）、protobuf（`proto/cypher_ddl.proto`、`proto/physical.proto`）、Apache Arrow（外部数据源 scan）、glog
**Storage**: 复用现有 `VertexTable`/`EdgeTable`/CSR 容器，通过 Schema 标记绕过 `Dump()`/`Checkpoint`，析构时随 Connection 回收
**Testing**: 单元测试（`tests/`）+ 端到端测试

## Project Structure

```text
proto/
├── cypher_ddl.proto                       # 变更: CreateVertexSchema/CreateEdgeSchema 添加 bool temporary
└── physical.proto                         # 已有 ExecutionFlag.create_temp_table，无需变更

include/neug/storages/graph/
├── schema.h                               # 变更: VertexSchema/EdgeSchema 添加 temporary 字段 + 查询方法
└── operation_params.h                     # 变更: CreateVertexTypeParam/CreateEdgeTypeParam 添加 temporary

src/storages/graph/
├── schema.cc                              # 变更: 序列化策略（to_yaml 包含, Serialize/DumpToYaml 跳过）
└── property_graph.cc                      # 变更: Dump/Open/Compact 跳过 temp; CreateEdgeType 边约束检查

src/execution/execute/ops/ddl/
├── create_vertex_type.cc                  # 变更: 从 proto 读取 temporary flag 传递到 builder
└── create_edge_type.cc                    # 变更: 同上

src/compiler/antlr4/
└── Cypher.g4                              # 变更: 新增 nEUG_LoadNodeTable / nEUG_LoadEdgeTable 规则

src/compiler/parser/
└── load_as.h                              # 新增: LoadAs Statement 类

src/compiler/binder/bind/
└── bind_load_as.cpp                       # 新增: bindLoadAs（label 冲突、类型绑定、模式门禁）

src/compiler/planner/
└── plan_load_as.cpp                       # 新增: planLoadAs → PhysicalPlan

include/neug/compiler/gopt/
└── g_physical_analyzer.h                  # 变更: 新增 LogicalLoadAs 分支设置 flags

src/compiler/gopt/
├── g_ddl_converter.cpp                    # 变更: convertLoadAs 生成算子序列
└── g_query_converter.cpp                  # 可能变更: 处理 LoadAs 的 DataSource + BatchInsert

src/main/
└── connection.cc                          # 变更: Close() 添加 temp label 清理 + 缓存刷新

include/neug/storages/graph/
└── graph_interface.h                      # 不变: 所有接口签名不变，临时图完全透明
```

**Structure Decision**: 改动严格限制在 (a) Schema 标记、(b) Proto 扩展、(c) 编译器前端新增 LoadAs、(d) 执行层传递 flag、(e) Connection 清理。不修改任何现有 AP 算子（scan/filter/project/join/dml）的核心逻辑——它们通过 PropertyGraph 统一访问临时和持久化数据。

## Data Model

This feature has the following data models:
1. **VertexSchema / EdgeSchema temporary 标记**
2. **CreateVertexTypeParam / CreateEdgeTypeParam temporary 扩展**
3. **CreateVertexSchema / CreateEdgeSchema Proto 扩展**
4. **LoadAs Statement**

---

### VertexSchema / EdgeSchema temporary 标记

**Data Structure**:

```cpp
// schema.h
struct VertexSchema {
  // ... 现有字段（label, properties, primary_key 等）...
  bool temporary = false;   // 新增：标记该类型是否为临时
};

struct EdgeSchema {
  // ... 现有字段 ...
  bool temporary = false;   // 新增
};
```

**Data Access & Update**:

- **查询是否临时**: `Schema::is_vertex_label_temporary(label_t)` / `is_edge_label_temporary(label_t, label_t, label_t)`
- **获取所有临时 labels**: `Schema::get_temporary_vertex_labels()` → `vector<label_t>`，`get_temporary_edge_labels()` → `vector<uint32_t>`
- **创建时设置**: `Schema::AddVertexLabel(...)` 扩展 `bool temporary = false` 参数
- **序列化策略**: `to_yaml()` 包含 temporary（compiler 用）；`Serialize()`/`DumpToYaml()` 跳过 temporary（持久化用）

---

### CreateVertexTypeParam / CreateEdgeTypeParam temporary 扩展

**Data Structure**:

```cpp
// operation_params.h
class CreateVertexTypeParam {
  // ... 现有字段 ...
  bool temporary = false;   // 新增
public:
  bool IsTemporary() const { return temporary; }
};

class CreateVertexTypeParamBuilder {
  // ... 现有方法 ...
  CreateVertexTypeParamBuilder& Temporary(bool temp) {  // 新增
    config.temporary = temp;
    return *this;
  }
};
// CreateEdgeTypeParam / CreateEdgeTypeParamBuilder 同理
```

**Data Access & Update**:

- 由 `CreateVertexTypeOpr` 构造 builder 时调用 `.Temporary(is_temporary_)` 设置
- `PropertyGraph::CreateVertexType()` 内部读取 `config.IsTemporary()` 设置 `VertexSchema::temporary = true`
- `PropertyGraph::CreateEdgeType()` 内部读取 `config.IsTemporary()` 并校验边的 temp 约束（src/dst 是 temp label → 边也必须是 temp）

---

### CreateVertexSchema / CreateEdgeSchema Proto 扩展

**Data Structure**:

```protobuf
// cypher_ddl.proto
message CreateVertexSchema {
    common.NameOrId vertex_type = 1;
    repeated PropertyDef properties = 2;
    repeated string primary_key = 3;
    ConflictAction conflict_action = 4;
    bool temporary = 5;                   // 新增
}

message CreateEdgeSchema {
    // ... 现有字段 ...
    bool temporary = 10;                  // 新增
}
```

**Data Access & Update**:

- 由 Planner/GDDLConverter 在生成 PhysicalPlan 时设置 `temporary = true`
- 由 `CreateVertexTypeOpr` 在执行时从 proto 读取 `temporary` 并传递给 `CreateVertexTypeParamBuilder`
- 增删改查数据的 proto（`BatchInsertVertex`/`InsertVertex`/`SetVertexProperty`/`DeleteVertex`/`Scan` 等）不需要任何改动——temporary 是 schema 级别概念，不是数据级别概念

---

### LoadAs Statement

**Data Structure**:

```cpp
// load_as.h
class LoadAs : public Statement {
  std::unique_ptr<BaseScanSource> source;        // 文件源（复用现有）
  std::string target_label;                      // AS 后的 label 名
  bool is_edge = false;                          // NODE TABLE vs EDGE TABLE
  // 通用 options
  bool header = true;
  bool auto_detect = true;
  // 节点 options
  std::string primary_key;                       // primary_key = "col"
  // 边 options
  std::string from_label;                        // from = "SrcLabel"
  std::string to_label;                          // to = "DstLabel"
  std::string from_col;                          // from_col = "src_id"
  std::string to_col;                            // to_col = "dst_id"
  // Filter pushdown (WHERE)
  std::unique_ptr<ParsedExpression> where_clause; // WHERE <predicate>，可选
  // Projection pushdown (RETURN)
  std::vector<std::string> return_columns;        // RETURN col1, col2，可选；空 = 全部列
};
```

**WHERE/RETURN 语义**:

- **WHERE**: 对数据源做行过滤（filter pushdown）。WHERE 中引用但不在 RETURN 中的列仅用于过滤，不成为 vertex/edge property。
- **RETURN**: 指定投影列（projection pushdown），仅 RETURN 中列出的列成为 vertex/edge property。若指定了 RETURN：
  - 节点: MUST 显式包含 `primary_key` 列，否则报错
  - 边: MUST 显式包含 `from_col` 和 `to_col` 列，否则报错
- 若未指定 RETURN，所有列成为 property（WHERE-only 列仍仅用于过滤）。

**Data Access & Update**:

- 由 Parser 的 `transformLoadNodeTable()` / `transformLoadEdgeTable()` 从 ANTLR 上下文构造
- 由 Binder 的 `bindLoadAs()` 校验（label 冲突、类型绑定、边约束、WHERE 表达式绑定、RETURN 列校验）
- 由 Planner 的 `planLoadAs()` 转化为 PhysicalPlan（在 DataSource 上设置 filter/projection）

## Algorithm Model

This feature has the following algorithm models:
1. **LOAD AS 执行流水线**
2. **Connection Close 清理流程**

---

### Algorithm 1: LOAD AS 执行流水线

**Algorithm Target**: 把一条 `LOAD FROM ... AS ...` 从解析到数据可查询的完整路径。

**Algorithm Details**:

```text
LOAD NODE TABLE FROM 'users.csv' (primary_key = 'user_id')
  WHERE age > 18 RETURN user_id, name, age AS TempUser
  ↓
1. [Parser]
   Cypher.g4 匹配 nEUG_LoadNodeTable 规则
   transformLoadNodeTable() → 生成 LoadAs Statement 实例 (is_edge=false)
   - 解析 WHERE 子句 → where_clause (ParsedExpression)
   - 解析 RETURN 列表 → return_columns = ["user_id", "name", "age"]
  ↓
2. [Binder] bindLoadAs()
   - 检查 label 名不与现有 persistent/temp label 冲突
   - 绑定列定义的类型
   - 绑定 WHERE 表达式（解析列引用、类型检查）
   - 校验 RETURN 列: 如指定了 RETURN，必须包含 primary_key 列（节点）或 from_col/to_col（边）
   - 如果是边，验证 src/dst label 存在；校验 temp 约束
   注: 模式门禁不在 Binder 层（ClientContext 无 access mode 信息），
       由 QueryProcessor::check_and_retrieve_pipeline() 统一拦截
  ↓
3. [Planner] planLoadAs()
   → 生成 LogicalPlan
   → GPhysicalAnalyzer 分析出 flag: { schema=true, batch=true, create_temp_table=true }
   → GDDLConverter/GQueryConverter 转化为 PhysicalPlan:
     [CreateVertexSchema(temporary=true, properties=RETURN列)]
       → [DataSource(filter=WHERE, projection=RETURN列∪WHERE列)]
       → [BatchInsertVertex]
   注: DataSource 的 projection 包含 WHERE 中引用的列（用于过滤），
       但 CreateVertexSchema 的 properties 仅包含 RETURN 列（最终成为 vertex property）
  ↓
4. [Execution]
   - CreateVertexTypeOpr: 从 proto 读取 temporary=true → builder.Temporary(true) → storage.CreateVertexType()
   - DataSource: 读取外部文件，应用 filter pushdown (WHERE) 和 projection pushdown (RETURN∪WHERE列)
   - BatchInsertVertex: 写入 vertex table（不关心 temporary），仅写入 RETURN 列
   - 失败回滚：如果 BatchInsertVertex 失败，调用 DeleteVertexType 清理已创建的类型
  ↓
5. [QueryProcessor] update_compiler_meta_if_needed()
   - 检测到 create_temp_table flag
   - 调用 schema_.to_yaml()（包含 temp labels）刷新 Catalog
   - 清空 query cache
  ↓
6. 后续 MATCH 查询自动看到 temp labels
```

边的 LOAD 流程同理，替换为 `CreateEdgeSchema` + `BatchInsertEdge`。

---

### Algorithm 2: Connection Close 清理流程

**Algorithm Target**: 确保 Connection 关闭时所有临时数据完全回收，缓存计划不失效。

**Algorithm Details**:

```text
Connection::Close()
  ↓
1. 检查 is_closed_ flag，避免重复清理
  ↓
2. 清理临时边（先边后点，避免引用完整性问题）
   for each temp_edge in schema.get_temporary_edge_labels():
     try: graph_.DeleteEdgeType(src, dst, edge)
     catch: log warning, continue    // per-label 错误隔离
  ↓
3. 清理临时点
   for each temp_vertex in schema.get_temporary_vertex_labels():
     try: graph_.DeleteVertexType(label)
     catch: log warning, continue
  ↓
4. 刷新查询缓存
   schema_yaml = graph_.schema().to_yaml()
   query_processor_->clear_cache(schema_yaml)
  ↓
5. is_closed_ = true
```

单 Connection + 同步阻塞 `Query()` 模型下，Close 被调用时不可能有正在执行的查询，不需要 abort in-flight query。

## Risks & Open Questions

- **LOAD 失败回滚粒度**: CreateVertexType 成功但 BatchAddVertices 中途失败时，需要可靠地清理已创建的 type 和部分写入的数据。当前 PropertyGraph 的 DeleteVertexType 是否能正确处理"有部分数据"的 table？需要验证。
- **`to_yaml()` 包含 temp labels 的性能**: 每次 LOAD AS 后 `to_yaml()` 会重新序列化整个 schema。如果 schema 很大（大量 persistent labels），这个开销需要评估。
- **query cache 全量失效**: 每次 LOAD AS / DROP / DROP ALL TEMP 都失效全量缓存。短期可接受；后续可改为"persistent schema 版本 + temp schema 版本"两段式 cache key。
