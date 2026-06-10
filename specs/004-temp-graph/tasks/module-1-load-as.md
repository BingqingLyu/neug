# Module 1: LOAD AS 语法与临时数据接入

**Goal**: 提供完整的 `LOAD NODE TABLE` / `LOAD EDGE TABLE` 语句支持，把外部文件物化为会话内的临时节点/边表，并确保临时数据不写入磁盘。

**Assignee**: TBD
**Label**: temp-graph
**Milestone**: TBD
**Project**: TBD

## [F008-T101] Schema 层添加 temporary 标记

**description**: 在 VertexSchema/EdgeSchema 上添加 `bool temporary` 字段，并在 Schema 类上提供查询和过滤临时 labels 的方法。这是整个临时图功能的基础数据模型。

**details**:
* 文件: `include/neug/storages/graph/schema.h`, `src/storages/graph/schema.cc`
* `VertexSchema` 和 `EdgeSchema` 结构体各添加 `bool temporary = false;` 字段
* `Schema` 类添加查询方法:
  - `bool is_vertex_label_temporary(label_t label) const;`
  - `bool is_edge_label_temporary(label_t src, label_t dst, label_t edge) const;`
  - `std::vector<label_t> get_temporary_vertex_labels() const;`
  - `std::vector<uint32_t> get_temporary_edge_labels() const;`
* `AddVertexLabel` / `AddEdgeLabel` 扩展签名，支持传入 `temporary` 参数
* 序列化策略:
  - `to_yaml()` **包含** temporary labels（compiler 需要看到它们做 binder 解析）
  - `Serialize()` / `DumpToYaml()` **跳过** temporary labels（持久化用，不写磁盘）
* 在 `to_yaml()` 输出中添加 `temporary: true` 标记，使 compiler 能识别

## [F008-T102] PropertyGraph 层 Dump/Open/Compact 防护

**description**: 确保 PropertyGraph 的持久化操作（Dump/Open/Compact）跳过 temporary labels，防止临时数据写入磁盘，并在 Open 时兜底清理残留。

**details**:
* 文件: `src/storages/graph/property_graph.cc`
* `Dump()`: 遍历 vertex_tables_ 和 edge_tables_ 时，检查 schema 的 `temporary` 标记，跳过 temporary labels
* `Compact()`: 同样跳过 temporary labels
* `DumpSchema()`: 由 T101 保证（Serialize 跳过 temporary）
* `Open()`: 加载 Schema 后检测是否有 temporary labels，如有则 LOG(WARNING) 并清理（兜底防护，正常情况下不应出现）
* 依赖: T101

## [F008-T103] CreateTypeParam 添加 temporary 字段

**description**: 在 CreateVertexTypeParam/CreateEdgeTypeParam 及其 Builder 上扩展 temporary 字段，使创建类型时能传递临时标记到 PropertyGraph。

**details**:
* 文件: `include/neug/storages/graph/operation_params.h`
* `CreateVertexTypeParam` 添加 `bool temporary = false;` 字段 + `bool IsTemporary() const` getter
* `CreateVertexTypeParamBuilder` 添加 `.Temporary(bool temp)` builder 方法
* `CreateEdgeTypeParam` / `CreateEdgeTypeParamBuilder` 同理
* `PropertyGraph::CreateVertexType()` 内部根据 `config.IsTemporary()` 设置 `VertexSchema::temporary = true`
* `PropertyGraph::CreateEdgeType()` 内部:
  - 根据 `config.IsTemporary()` 设置 `EdgeSchema::temporary = true`
  - 边约束检查: 如果 src 或 dst 是 temp label，当前边也必须标记为 temp
* 依赖: T101

## [F008-T104] Proto 扩展 CreateVertexSchema/CreateEdgeSchema

**description**: 在 cypher_ddl.proto 的 CreateVertexSchema 和 CreateEdgeSchema message 中添加 `bool temporary` 字段。CRUD 数据 proto 不需要改动。

**details**:
* 文件: `proto/cypher_ddl.proto`
* `CreateVertexSchema` 添加 `bool temporary = 5;`
* `CreateEdgeSchema` 添加 `bool temporary = 10;`（field number 根据现有字段确定）
* 已有的 `ExecutionFlag.create_temp_table`（`proto/physical.proto` field 6）直接复用，无需变更
* 数据操作 proto（`BatchInsertVertex`, `InsertVertex`, `SetVertexProperty`, `DeleteVertex`, `Scan` 等）不需要任何改动——temporary 是 schema 级别概念

## [F008-T105] 执行层传递 temporary 标记

**description**: 执行算子 CreateVertexTypeOpr/CreateEdgeTypeOpr 从 PhysicalPlan proto 读取 temporary flag，传递到 ParamBuilder，完成 Schema→Proto→Execution→Storage 的完整链路。

**details**:
* 文件: `src/execution/execute/ops/ddl/create_vertex_type.cc`, 对应 edge 文件
* `CreateVertexTypeOpr` 构造时从 proto 的 `CreateVertexSchema.temporary` 读取并存储为成员 `is_temporary_`
* `Eval()` 中传递: `builder.Temporary(is_temporary_)`
* `CreateEdgeTypeOpr` 同理
* 失败回滚: 如果 LOAD AS 流程中 `BatchAddVertices` 失败，需调用 `DeleteVertexType` 回滚已创建的 type
* 数据加载算子（`BatchAddVertices`/`BatchAddEdges`）无需改动——它们不关心 temporary 标记
* 依赖: T103, T104

## [F008-T106] Parser — LoadAs Statement 与 Cypher.g4 语法

**description**: 在 Cypher 语法中新增 `LOAD NODE TABLE` / `LOAD EDGE TABLE` 规则（含可选 WHERE/RETURN 子句），创建独立的 LoadAs Statement 类。LoadAs 是 DDL 操作（创建类型+写入图），与现有 LoadFrom ReadingClause（返回行）语义完全不同。

**details**:
* 文件: `src/compiler/antlr4/Cypher.g4`, `src/compiler/parser/load_as.h`（新增）
* Cypher.g4 新增规则:
  - `nEUG_LoadNodeTable`: `LOAD NODE TABLE FROM <source> (<options>) [WHERE <predicate>] [RETURN <columns>] AS <label>`
  - `nEUG_LoadEdgeTable`: `LOAD EDGE TABLE FROM <source> (<options>) [WHERE <predicate>] [RETURN <columns>] AS <label>`
* 节点 options: `primary_key`, `header`, `auto_detect` 等 key=value 对
* 边 options: `from`, `to`, `from_col`, `to_col`, `header`, `auto_detect` 等
* WHERE 子句: 可选，解析为 `ParsedExpression`，用于数据源行过滤（filter pushdown）
* RETURN 子句: 可选，解析为 `std::vector<std::string>` 列名列表，用于投影（projection pushdown）
* 新增 `LoadAs` Statement 类，包含:
  - `std::unique_ptr<BaseScanSource> source` — 文件源（复用现有）
  - `std::string target_label` — AS 后的 label 名
  - `bool is_edge` — NODE TABLE vs EDGE TABLE
  - 节点相关: `std::string primary_key`
  - 边相关: `std::string from_label` / `to_label` / `from_col` / `to_col`
  - 通用: `bool header`, `bool auto_detect`
  - `std::unique_ptr<ParsedExpression> where_clause` — 可选 WHERE 谓词
  - `std::vector<std::string> return_columns` — 可选 RETURN 列表（空 = 全部列）
* 文件源解析从 LoadFrom 提取公共函数复用
* transformer 从 ANTLR context 构造 `LoadAs` 实例（含 WHERE/RETURN 解析）

## [F008-T107] Binder — bindLoadAs 校验

**description**: 新增 bindLoadAs 函数，校验 LOAD AS 的合法性，包括 label 冲突、类型绑定、边约束、WHERE 表达式绑定和 RETURN 列校验。

**details**:
* 文件: `src/compiler/binder/bind/bind_load_as.cpp`（新增）
* 校验逻辑:
  - label 冲突: 检查 label 名不与现有 persistent/temp label 冲突
  - 类型绑定: 绑定列定义的数据类型
  - 边约束: 如果是边，验证 src/dst label 存在；验证涉及临时点的边也必须是临时边
  - WHERE 表达式绑定: 解析列引用，校验类型合法性；WHERE 中引用但不在 RETURN 中的列仅用于过滤，不成为 vertex/edge property
  - RETURN 列校验:
    * 若指定了 RETURN，必须显式包含 primary_key 列（节点）或 from_col/to_col 列（边），否则报错
    * 校验 RETURN 中所有列名在源文件中存在
    * 仅 RETURN 列成为最终 vertex/edge property
  - 若未指定 RETURN，所有源文件列成为 property
* 注意: 模式门禁（AP READ_WRITE 检查）不在 Binder 层——Binder 的 `ClientContext` 没有 access mode 信息。模式门禁由 `QueryProcessor::check_and_retrieve_pipeline()` 统一处理：`is_read_only_` flag + `ExecutionFlag.create_temp_table` 已能拦截 read-only 连接；TP service 是独立路径，天然不支持 LOAD AS。
* 生成 `BoundLoadAs` 传递给 Planner
* 依赖: T106

## [F008-T108] Planner — planLoadAs 与 PhysicalPlan 生成

**description**: 新增 planLoadAs 将 BoundLoadAs 转化为 PhysicalPlan，经 GPhysicalAnalyzer 设置 ExecutionFlag，由 GDDLConverter/GQueryConverter 生成最终算子序列。支持 WHERE filter pushdown 和 RETURN projection pushdown。

**details**:
* 文件: `src/compiler/planner/plan_load_as.cpp`（新增）, `include/neug/compiler/gopt/g_physical_analyzer.h`, `src/compiler/gopt/g_ddl_converter.cpp`, `src/compiler/gopt/g_query_converter.cpp`
* `planLoadAs()` 生成 LogicalPlan
* `GPhysicalAnalyzer` 新增 `LogicalLoadAs` 分支，设置 flags:
  - `flag.schema = true`
  - `flag.batch = true`
  - `flag.create_temp_table = true`
* `GDDLConverter::convertLoadAs()` 生成算子序列:
  - 节点: `[CreateVertexSchema(temporary=true, properties=RETURN列)]` → `[DataSource(filter=WHERE, projection=RETURN列∪WHERE列)]` → `[BatchInsertVertex]`
  - 边: `[CreateEdgeSchema(temporary=true, properties=RETURN列)]` → `[DataSource(filter=WHERE, projection=RETURN列∪WHERE列)]` → `[BatchInsertEdge]`
* WHERE/RETURN 处理:
  - `CreateVertexSchema`/`CreateEdgeSchema` 的 properties 仅包含 RETURN 列（最终成为 vertex/edge property）
  - `DataSource` 的 projection 包含 RETURN 列 + WHERE 中引用的列（用于过滤）
  - `DataSource` 的 filter 为 WHERE 谓词表达式
  - 若未指定 RETURN，properties 和 projection 均为源文件全部列
  - 若未指定 WHERE，DataSource 不设置 filter
* 复用现有 `QueryProcessor::update_compiler_meta_if_needed()`，检测到 `create_temp_table` flag 后自动刷新 Catalog + 清空 query cache
* 依赖: T104, T105, T107
