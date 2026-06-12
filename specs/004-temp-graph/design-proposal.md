# 临时图 (Temporary Graph) 设计方案

**创建时间**: 2026-06-10
**作者**: BingqingLyu
**状态**: 供讨论

## 1. 功能范围

### 1.1 第一版支持

| 能力           | 说明                                                         |
| -------------- | ------------------------------------------------------------ |
| 加载临时点表   | `LOAD NODE TABLE FROM <file> (<options>) AS <label>`         |
| 加载临时边表   | `LOAD REL TABLE FROM <file> (<options>) AS <label>`         |
| 加载时过滤投影 | `WHERE <expr> RETURN <cols>` 在加载阶段过滤行和选择列，减少内存占用 |
| 统一查询       | 临时 label 与持久化 label 在同一条 Cypher 中自由关联         |
| 显式删除       | 复用现有 `DROP TABLE <label>`                                |
| 自动清理       | Connection 关闭时自动清理所有临时 label + 刷新查询缓存       |
| 后续写入       | 临时图加载后支持 INSERT/UPDATE/DELETE                        |
| 数据源         | CSV / JSON / Parquet（复用现有 LOAD FROM 的数据源支持）      |

### 1.2 第一版不支持

| 能力                     | 原因                                                                                             |
| ------------------------ | ------------------------------------------------------------------------------------------------ |
| READ_ONLY 模式下的临时图 | READ_WRITE 模式只有单 Connection，天然满足隔离；READ_ONLY 多 Connection 需额外隔离机制，延后处理 |
| Connection 隔离          | 同上，第一版不需要                                                                               |
| 事务语义                 | AP 场景（加载→分析→丢弃），不需要事务回滚                                                       |
| DDL（CREATE/ALTER）      | 临时类型通过 LOAD 一次性加载，仅支持 LOAD 创建 + DML 修改                                       |
| 名称遮蔽                 | 临时 label 名不能与持久化 label 名重复                                                           |
| 索引                     | 临时点/边上不支持建索引，延后处理                                                                |

### 1.3 约束

1. **Label 名唯一性**：临时 label 名不能与持久化 label 名重复
2. **涉及临时点的边必须是临时边**：持久化边不能引用临时点（session 结束后临时点被清理，持久化边会悬空）
3. **临时边可自由引用临时或持久化点**：`FROM TEMP TO PERSIST`、`FROM PERSIST TO TEMP`、`FROM TEMP TO TEMP` 均合法

### 1.4 未来兼容性

**AP 链路引入 Transaction**

未来 AP 链路的所有查询和 DDL 操作将通过 Transaction 处理（与 TP 链路对齐）。在该方案下，Transaction 需要跳过临时图信息——不为临时数据的写入（LOAD AS、DML）生成 WAL 记录，避免污染持久化数据库。

当前共享存储方案的兼容路径：临时类型通过 `VertexSchema/EdgeSchema.temporary` 标记区分，Transaction 层可据此决定是否跳过 WAL 写入。Schema 标记已为这一扩展预留了判断依据。

**索引支持**

临时点表当前不支持索引。未来如需对临时 label 建索引（如 hash index on primary key），可复用现有索引机制，通过 `temporary` 标记控制索引的生命周期（随 Connection 清理）和持久化行为（跳过 Dump）。

## 2. 用户接口

### 2.1 基本语法

```cypher
-- 加载临时点表
LOAD NODE TABLE FROM 'persons.csv' (primary_key = 'id') AS TempPerson

-- 加载临时边表（临时点 → 持久化点）
LOAD REL TABLE FROM 'purchases.csv' (
  from = 'TempPerson', to = 'Product',
  from_col = 'buyer_id', to_col = 'product_id'
) AS TempPurchased
```

### 2.2 加载时过滤与投影

LOAD AS 支持可选的 `WHERE` 和 `RETURN` 子句，在数据加载阶段进行过滤和列选择，减少写入内存的数据量：

```cypher
-- 只加载 amount > 100 的行，且只保留 buyer_id 和 product_id 两列
LOAD REL TABLE FROM '/data/purchases.csv' (
  from = 'TempUser', to = 'Product',
  from_col = 'buyer_id', to_col = 'product_id'
)
WHERE amount > 100
RETURN buyer_id, product_id
AS FilteredPurchased
```

- **WHERE**（可选）：过滤表达式，作用于源文件的行。只有满足条件的行才会被加载到临时表中。表达式中的列名引用源文件的列。
- **RETURN**（可选）：投影列列表，指定哪些列写入临时表。未列出的列不加载。省略时加载所有列。`from_col`/`to_col`（边）和 `primary_key`（点）引用的列必须包含在 RETURN 列表中。

**实现方式**：WHERE 和 RETURN 在 DataSource 算子阶段处理——DataSource 读取文件时应用过滤条件和列投影，只输出匹配的行和选定的列给下游 BatchInsert 算子。复用现有 `LOAD FROM` 的 `oC_Where` 解析和 `bindWhereExpression()` 绑定模式。

### 2.3 查询与删除

```cypher
-- 查询（临时 + 持久化统一视图）
MATCH (a:TempPerson)-[:TempPurchased]->(b:Product)-[:BELONGS_TO]->(c:Category)
RETURN a.name, b.name, c.name

-- 显式删除（复用现有 DDL）
DROP TABLE TempPerson
DROP TABLE TempPurchased
```

### 2.4 Options 参考

**LOAD NODE TABLE** options：
- `primary_key`（必选）：指定主键列名
- `header`（可选，默认 true）：文件是否含表头
- `auto_detect`（可选，默认 true）：自动推断列类型

**LOAD REL TABLE** options：
- `from` / `to`（必选）：指定源/目标点类型名（可以是临时或持久化类型）
- `from_col` / `to_col`（必选）：指定源/目标外键列名
- `header`、`auto_detect`（同上）

## 3. 设计方案

### 3.1 总体方案：共享存储 + Schema 标记

临时数据与持久化数据共存于同一个 PropertyGraph 实例，通过 `VertexSchema`/`EdgeSchema` 上的 `temporary` 标记区分。

```
PropertyGraph（单实例）
├── Schema
│   ├── VertexSchema: Person     (temporary=false)
│   ├── VertexSchema: TempPerson (temporary=true)     ← 临时
│   ├── EdgeSchema:   KNOWS      (temporary=false)
│   └── EdgeSchema:   TempPurchased (temporary=true)  ← 临时
├── VertexTable[]: Person 表 + TempPerson 表 共存
└── EdgeTable[]:   KNOWS 表 + TempPurchased 表 共存
```

**核心思路**：schema 里带上标记（`temporary=true`），但存储层的数据结构（VertexTable、EdgeTable、CSR）完全不变。临时类型和持久化类型在同一个 label 空间和 VID 空间中，查询路径零改动。差异仅体现在：
- **写入路径**：Dump/Serialize/Checkpoint 按标记跳过临时 labels
- **生命周期**：Connection 关闭时按标记清理临时 labels

选择共享存储而非独立存储（DuckDB 路线）的原因详见 [storage-design-comparison.md](./storage-design-comparison.md)。

### 3.2 端到端 Workflow

```
用户执行: LOAD NODE TABLE FROM 'users.csv' (primary_key = 'user_id')
         WHERE age > 18 RETURN user_id, name AS TempUser
  │
  ▼
[Parser]
  Cypher.g4 匹配 nEUG_LoadNodeTable 规则
  transformLoadNodeTable() → 生成 LoadAs Statement（is_rel=false, where=age>18, return=[user_id,name]）
  │
  ▼
[Binder] bindLoadAs()
  ① 检查 label 名不与现有 persistent/temp label 冲突
  ② 绑定列定义的类型（auto_detect 推断或显式指定）
  ③ 如果有 WHERE：绑定过滤表达式；如果有 RETURN：校验列名 + primary_key 必须包含
  ④ 如果是边：验证 from/to label 存在 + 校验 temp 约束
  │
  ▼
[Planner] planLoadAs()
  生成 LogicalPlan → GPhysicalAnalyzer 设置 flags:
    { schema=true, batch=true, create_temp_table=true }
  → GDDLConverter 生成 PhysicalPlan 算子序列:
    [CreateVertexSchema(temporary=true)] → [DataSource] → [BatchInsertVertex]
  │
  ▼
[Execution]
  ① CreateVertexTypeOpr: 从 proto 读取 temporary=true
     → builder.Temporary(true) → PropertyGraph::CreateVertexType()
     → Schema 中创建 VertexSchema(temporary=true) + 分配 VertexTable
  ② DataSource: 读取外部文件（CSV/Parquet/JSON），应用 WHERE 过滤 + RETURN 投影
  ③ BatchInsertVertex: 写入 VertexTable（不关心 temporary 标记）
  ④ 如果 ③ 失败：调用 DeleteVertexType 回滚 ① 创建的 schema 和 table
  │
  ▼
[QueryProcessor] update_compiler_meta_if_needed()
  检测到 create_temp_table flag
  → 调用 schema_.to_yaml()（包含 temp labels）刷新 Catalog
  → 清空 query cache
  │
  ▼
后续 MATCH 查询自动看到 TempUser，与持久化 labels 统一查询
  │
  ▼
Connection::Close()
  → 遍历清理所有 temporary labels（先边后点）
  → 刷新查询缓存
```

## 4. 各层改动

### 4.1 改动概览

**结构性改动**：在 Schema 的 `VertexSchema`/`EdgeSchema` 上添加 `bool temporary` 标记。仅此一项结构变更，存储层的数据结构（VertexTable、EdgeTable、CSR、VID 索引等）完全不变。

**各层改动原则**：
- **新增的部分**：Parser 新增 LoadAs 语句、Binder/Planner 新增对应处理
- **扩展的部分**：Proto 和 CreateTypeParam 添加 temporary 字段、执行算子传递该字段
- **防护的部分**：Dump/Serialize/Compact 跳过临时 labels、Connection::Close() 清理
- **不变的部分**：查询算子、图接口层、数据加载算子——对临时图完全透明

### 4.2 Parser 层

**新增**：`LOAD NODE TABLE` / `LOAD REL TABLE` 语法规则与 `LoadAs` Statement 类。

- `Cypher.g4`：新增 `nEUG_LoadNodeTable` 和 `nEUG_LoadRelTable` 规则，包含可选的 `oC_Where` 和 `RETURN` 子句
- `load_as.h`（新增）：`LoadAs` Statement 类，包含 source、target_label、is_rel、primary_key、from/to/from_col/to_col、wherePredicate（可选）、returnColumns（可选）等字段
- Transformer：新增 `transformLoadNodeTable()` / `transformLoadRelTable()`，文件源解析复用现有 `transformScanSource()`，WHERE 解析复用现有 `oC_Where` 规则

**设计决策**：LoadAs 作为独立 Statement 实现，不复用现有 `LoadFrom` ReadingClause。原因是 `LoadFrom` 返回行（SELECT 语义），`LoadAs` 创建类型并写入图（DDL + 数据加载语义），两者完全不同。WHERE/RETURN 的语法复用现有 Cypher 基础规则，但语义不同：它们作用于源文件数据，而非图数据。

### 4.3 Compiler 层（Binder + Planner）

**Binder — 新增 `bindLoadAs()`**：
- Label 冲突检查：label 名不与 persistent/temp label 重复
- 类型绑定：绑定列定义的数据类型
- 边约束校验：from/to label 存在 + 涉及临时点的边必须是临时边
- WHERE 绑定：如有 WHERE 子句，调用 `bindWhereExpression()` 绑定过滤表达式（复用现有 LOAD FROM 模式）
- RETURN 绑定：如有 RETURN 子句，校验列名存在于源文件列中，且 primary_key / from_col / to_col 必须包含在内

注：模式门禁（校验 AP READ_WRITE）不在 Binder 层处理——Binder 不感知 connection 模式，由 Execution 层（QueryProcessor `is_read_only_` 检查 + `create_temp_table` flag）负责拦截。

**Planner — 新增 `planLoadAs()`**：
- 生成 LogicalPlan
- `GPhysicalAnalyzer` 新增 `LogicalLoadAs` 分支，设置 `schema + batch + create_temp_table` flags
- `GDDLConverter::convertLoadAs()` 生成算子序列：`CreateVertexSchema(temporary=true)` → `DataSource`（含 filter/projection）→ `BatchInsertVertex`

**不受影响**：
- `QueryProcessor::update_compiler_meta_if_needed()` — 已有 `create_temp_table` flag 处理，无需改动
- 所有现有查询编译路径 — `to_yaml()` 输出包含 temp labels 后，compiler 自动识别
- `GPhysicalConvertor` — 已有 `convertExecutionFlag()` 含 `create_temp_table`，无需改动

### 4.4 Execution 层

**扩展**：`CreateVertexTypeOpr` / `CreateEdgeTypeOpr` 从 proto 的 `CreateVertexSchema.temporary` / `CreateEdgeSchema.temporary` 读取标记，传递到 `CreateVertexTypeParamBuilder.Temporary(true)`。

**不受影响**：
- `BatchAddVertices` / `BatchAddEdges` — 只管写数据，不关心 temporary
- 所有查询算子（Scan/Filter/Project/Join/DML）— 通过 PropertyGraph 统一访问，对临时图完全透明

### 4.5 Storage 层

**Schema（schema.h / schema.cc）**：

| 改动         | 说明                                                                                   |
| ------------ | -------------------------------------------------------------------------------------- |
| 新增字段     | `VertexSchema.temporary`、`EdgeSchema.temporary`（`bool`，默认 `false`）               |
| 新增方法     | `is_vertex_label_temporary()`、`is_edge_label_temporary()`、`get_temporary_vertex_labels()`、`get_temporary_edge_labels()` |
| 扩展签名     | `AddVertexLabel()` / `AddEdgeLabel()` 新增 `bool temporary = false` 参数               |
| 序列化策略   | `to_yaml()` 包含 temporary labels（compiler 用）；`Serialize()` / `DumpToYaml()` 跳过 temporary labels（持久化用） |

**CreateTypeParam（operation_params.h）**：

| 改动         | 说明                                                                                   |
| ------------ | -------------------------------------------------------------------------------------- |
| 新增字段     | `CreateVertexTypeParam.temporary`、`CreateEdgeTypeParam.temporary`                      |
| 新增方法     | `IsTemporary()` getter、`Temporary(bool)` builder 方法                                 |

**PropertyGraph（property_graph.cc）**：

| 方法                  | 改动                                                                                              |
| --------------------- | ------------------------------------------------------------------------------------------------- |
| `CreateVertexType()`  | 根据 `config.IsTemporary()` 设置 `VertexSchema.temporary`                                        |
| `CreateEdgeType()`    | 根据 `config.IsTemporary()` 设置 + 校验边的 temp 约束（src/dst 是 temp → 边也必须是 temp）       |
| `Dump()`              | 遍历 vertex_tables_ / edge_tables_ 时跳过 `temporary` labels                                     |
| `Open()`              | 加载 Schema 后检测残留 temporary labels，LOG(WARNING) 并清理（兜底防护）                          |
| `Compact()`           | 跳过 temporary labels                                                                             |

**不受影响**：
- `BatchAddVertices()` / `BatchAddEdges()` — 不关心 temporary
- `DeleteVertexType()` / `DeleteEdgeType()` — 不区分 temp/persistent，统一处理
- `AddVertex()` / `AddEdge()` / `DeleteVertex()` / `DeleteEdge()` — DML 统一
- `GetGenericOutgoingGraphView()` / `GetGenericIncomingGraphView()` — 查询统一
- 图接口层 `StorageUpdateInterface` / `StorageAPUpdateInterface` — 所有接口签名不变，临时图完全透明

### 4.6 Proto 层

| Proto                  | 改动                                         |
| ---------------------- | -------------------------------------------- |
| `cypher_ddl.proto`     | `CreateVertexSchema` 新增 `bool temporary`；`CreateEdgeSchema` 新增 `bool temporary` |
| `physical.proto`       | 已有 `ExecutionFlag.create_temp_table`（field 6），直接复用，无需变更                |
| 数据操作 proto         | `BatchInsertVertex`、`InsertVertex`、`Scan` 等均无需改动——temporary 是 schema 级别概念 |

### 4.7 Connection 层

**`Connection::Close()` 新增临时数据清理**：

```
Close()
  → 检查 is_closed_ 避免重复清理
  → 清理临时边（先边后点，避免引用完整性问题）
     for each temp_edge: DeleteEdgeType()，per-label try-catch 错误隔离
  → 清理临时点
     for each temp_vertex: DeleteVertexType()，per-label try-catch 错误隔离
  → 刷新查询缓存（清除包含已删除 temp labels 的缓存计划）
  → is_closed_ = true
```

**不受影响**：`Query()` / `GetSchema()` / `IsClosed()` 公开接口不变。

### 4.8 关键决策

| 层       | 决策                              | 选择                                  | 理由                                                                               |
| -------- | --------------------------------- | ------------------------------------- | ---------------------------------------------------------------------------------- |
| 总体     | 存储架构                          | 共享存储 + Schema 标记                | 避免多图合并层，查询路径零改动；详见 storage-design-comparison.md                  |
| Parser   | LoadAs 定位                       | 独立 Statement，非 ReadingClause      | LoadFrom 返回行（SELECT），LoadAs 创建类型并写入图（DDL+数据），语义不同           |
| Execution| 模式门禁位置                      | Execution 层检查                      | Binder 不感知 connection 模式；由 QueryProcessor 的 `is_read_only_` + `create_temp_table` flag 拦截 |
| Execution| 加载接口                          | 复用 CreateVertexType + BatchAddVertices | Param 加 temporary 字段即可，不需要新方法                                         |
| Storage  | DROP 语法                         | 复用现有 `DROP TABLE`                 | 不区分 temp/persistent，统一处理                                                   |
| Storage  | to_yaml vs Serialize              | to_yaml 包含 temp，Serialize 跳过     | compiler 需要看到 temp labels，持久化不能包含                                      |
| Storage  | 引用完整性                        | 共享存储天然保证                      | 持久化点被 DROP 时，级联删除引用它的临时边，由 PropertyGraph 现有机制处理           |
| Connection| 清理方式                         | Close() 遍历清理 + 刷新缓存          | 复用现有 DeleteVertexType/DeleteEdgeType，先边后点                                  |

## 5. 容错保证

### 5.1 基本安全性

临时图的两条底线保证：

| 保证               | 机制                                                                                    |
| ------------------ | --------------------------------------------------------------------------------------- |
| 临时数据不写入磁盘 | `Dump()` / `Serialize()` / `DumpToYaml()` 按 `temporary` 标记跳过；临时数据不经过 WAL   |
| 内存可释放         | Connection::Close() 自动清理；进程退出时 OS 回收                                        |

### 5.2 按用户操作阶段的容错

#### 阶段 1：LOAD AS 执行

LOAD AS 由三个执行步骤组成：① CreateVertexType（创建 schema + table）→ ② DataSource（读取文件）→ ③ BatchInsertVertex（写入数据）。

| 失败点                             | 现象                     | 容错策略                                                                                              |
| ---------------------------------- | ------------------------ | ----------------------------------------------------------------------------------------------------- |
| ① CreateVertexType 失败            | Label 冲突、参数非法等   | Schema 未被修改，无残留，直接报错                                                                     |
| ② DataSource 失败（文件不存在等）  | Schema 已创建，数据未写入| 执行层捕获错误后调用 `DeleteVertexType()` 回滚已创建的 schema 和 table                                |
| ③ BatchInsertVertex 中途失败       | Schema 已创建，数据部分写入 | 同上，调用 `DeleteVertexType()` 清理（删除整个 VertexTable，包含已写入的部分数据）                   |
| 任意阶段进程 crash                 | 内存数据丢失             | 安全——临时数据未经 WAL/Dump，磁盘无残留。`Open()` 兜底检测残留 temp labels 并清理                    |

**关键点**：LOAD AS 要么完全成功（schema + 数据全部就绪），要么完全失败（无残留）。不存在"schema 创建了但没有数据"的中间状态暴露给用户。

#### 阶段 2：查询

| 场景                               | 容错策略                                                                                              |
| ---------------------------------- | ----------------------------------------------------------------------------------------------------- |
| 查询引用的临时 label 不存在        | 与持久化 label 相同的行为——compiler 报"未知 label"错误，而非返回空结果                                |
| 查询过程中进程 crash               | 安全——临时数据纯内存，无磁盘残留                                                                      |

#### 阶段 3：DDL（DROP）

| 场景                               | 容错策略                                                                                              |
| ---------------------------------- | ----------------------------------------------------------------------------------------------------- |
| `DROP TABLE TempX`                 | 复用现有 `DeleteVertexType()`/`DeleteEdgeType()`，不区分 temp/persistent                              |
| DROP 持久化点类型，被临时边引用    | 现有 `DeleteVertexType()` 级联删除引用该类型的所有边（含临时边），共享存储天然保证                     |
| DROP 后查询已删除的 temp label     | 报"未知 label"错误（查询缓存已在 DDL 后被刷新）                                                      |

#### 阶段 4：Connection 关闭

| 场景                               | 容错策略                                                                                              |
| ---------------------------------- | ----------------------------------------------------------------------------------------------------- |
| 正常 Close                         | 遍历清理所有 temp labels（先边后点）+ 刷新查询缓存                                                    |
| 单个 label 清理失败                | per-label try-catch 错误隔离，不阻断其他 label 清理，LOG(WARNING)                                     |
| Close 后新 Connection 查询同名 label| 查询缓存已刷新，不会命中旧缓存计划；label 已删除，返回"未知 label"                                  |
| 进程直接退出（未调用 Close）       | OS 回收内存，磁盘无残留。下次 `Open()` 兜底检测                                                      |

#### 阶段 5：Checkpoint / Dump

| 场景                               | 容错策略                                                                                              |
| ---------------------------------- | ----------------------------------------------------------------------------------------------------- |
| 临时数据存活期间触发 Checkpoint    | `Dump()` 跳过 temporary labels，磁盘快照不包含临时数据                                                |
| Checkpoint 后重启                  | 临时数据不在快照中，不会被恢复。`Open()` 兜底检测残留 temp labels                                     |

## 6. 接口变更汇总

### 新增接口

| 层       | 接口                                                   | 说明                                          |
| -------- | ------------------------------------------------------ | --------------------------------------------- |
| Schema   | `is_vertex_label_temporary(label_t)`                   | 查询 vertex label 是否临时                    |
| Schema   | `is_edge_label_temporary(label_t, label_t, label_t)`   | 查询 edge label 是否临时                      |
| Schema   | `get_temporary_vertex_labels()`                        | 获取所有临时 vertex labels                    |
| Schema   | `get_temporary_edge_labels()`                          | 获取所有临时 edge labels                      |
| Param    | `CreateVertexTypeParam::IsTemporary()`                 | 查询是否创建临时类型                          |
| Param    | `CreateVertexTypeParamBuilder::Temporary(bool)`        | 设置临时标记                                  |
| Parser   | `LoadAs` class                                         | 新 Statement 类型                             |
| Binder   | `BoundLoadAs` / `bindLoadAs()`                         | 绑定 LoadAs                                   |
| Planner  | `planLoadAs()`                                         | 生成 LoadAs 执行计划                          |
| Proto    | `CreateVertexSchema.temporary`                         | 新增 `bool temporary` 字段                    |
| Proto    | `CreateEdgeSchema.temporary`                           | 新增 `bool temporary` 字段                    |
| Compiler | `GPhysicalAnalyzer` 新增 `LogicalLoadAs` 分支          | 设置 schema + batch + create_temp_table flags |

### 不变接口

| 层             | 接口                                                      | 说明                                                           |
| -------------- | --------------------------------------------------------- | -------------------------------------------------------------- |
| PropertyGraph  | `BatchAddVertices()` / `BatchAddEdges()`                  | 不关心 temporary                                               |
| PropertyGraph  | `DeleteVertexType()` / `DeleteEdgeType()`                 | 不区分 temp/persistent                                         |
| PropertyGraph  | `AddVertex()` / `AddEdge()` / `DeleteVertex()`            | DML 统一                                                       |
| Connection     | `Query()` / `GetSchema()` / `IsClosed()`                  | 公开接口不变                                                   |
| QueryProcessor | `update_compiler_meta_if_needed()`                        | 已处理 create_temp_table                                       |
| ExecutionFlag  | `create_temp_table`                                       | 已有字段，直接复用                                             |
| 图接口层       | `StorageUpdateInterface` / `StorageAPUpdateInterface`     | 所有接口签名不变，临时图完全透明                               |
