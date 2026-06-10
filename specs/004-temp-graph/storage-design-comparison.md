# 临时图存储方案对比

**创建时间**: 2026-06-08
**作者**: BingqingLyu

## 概述

临时图的存储有两种基本方案：

- **方案 A**：独立存储 — 每个 connection 持有独立的临时 PropertyGraph 实例（DuckDB 路线）
- **方案 B**：共享存储 — 临时数据与持久化数据共存于同一个 PropertyGraph，通过 schema 标记区分

## 方案 A：独立存储（DuckDB 路线）

### 架构

```
Connection
├── PropertyGraph& graph_         (持久化图，共享引用)
└── PropertyGraph temp_graph_     (临时图，per-connection 独立实例，纯内存)
        ├── Schema
        ├── VertexTable[]
        └── EdgeTable[]

查询时通过合并层统一:
UnifiedGraphView
├── graph_ (持久化)
└── temp_graph_ (临时)
```

### DuckDB 的实现方式

DuckDB 每个 connection（ClientData）持有一个独立的 temp AttachedDatabase：

- 独立的 Catalog（schema）
- 独立的 StorageManager（IN_MEMORY_PATH，纯内存）
- 独立的 TransactionManager
- 通过 Catalog Search Path 在查询时统一两个 catalog 的查找

### 优势

1. **Connection 隔离天然解决**：每个 connection 的临时图是独立实例，其他 connection 看不到
2. **生命周期管理简单**：connection 关闭 → 销毁临时 PropertyGraph 实例 → 内存自动释放，无需逐个扫描清理
3. **Checkpoint 无风险**：只 checkpoint 持久化 PropertyGraph，不可能误写临时数据
4. **WAL 天然跳过**：临时 PropertyGraph 使用 DummyWalWriter（已有 no-op 实现），不需要在任何写入路径上做判断
5. **不污染持久化 Schema**：临时 label 存在独立 Schema 中，持久化 Schema 保持干净
6. **名称遮蔽可选**：两个独立 Schema 下同名 label 不冲突（虽然当前决定不做遮蔽）

### Schema 管理

每个 PropertyGraph 持有自己的 Schema（持久化图有持久化 Schema，临时图有临时 Schema）。查询时合并两个 Schema 构建统一的 GraphEntry：

```
Connection A 查询时的有效 schema:
  = 持久化 Schema（共享）+ Connection A 的临时 Schema

Connection B 查询时的有效 schema:
  = 持久化 Schema（共享）+ Connection B 的临时 Schema
```

临时 Schema 维护在 Connection 上（作为临时 PropertyGraph 的一部分），connection 销毁时自动消失。

### 劣势

1. **本质上是多图联合查询**：查询层需要同时访问两个 PropertyGraph 实例、合并 Schema、跨图解析 VID。这不仅仅是"临时图"功能，而是一整套多图基础设施——如果 NeuG 未来不需要多图能力，这部分基础设施就是纯粹为临时图付出的成本
2. **查询层需要合并**：需要实现 UnifiedGraphView（或类似机制）来合并两个 PropertyGraph 的扫描结果（scanFwd/scanBwd、vertex 读取等）
3. **跨图 VID 映射**：两个 PropertyGraph 的 label_t 独立编号。当临时边引用持久化点（如 `FROM TEMP_PERSON TO PERSON`）时，需要 EdgeEndpointMapping 解决跨图 VID 查找
4. **接口重复**：UnifiedGraphView 需要实现 PropertyGraph 已有的各种查询接口（Graph 接口的 scanFwd/scanBwd 等），带来维护成本
5. **NeuG PropertyGraph 较重**：PropertyGraph 包含 CSR 结构、VID 索引等，比 DuckDB 的 table 结构更复杂，实现合并层的成本更高

## 方案 B：共享存储 + Schema 标记

### 架构

```
Connection
└── PropertyGraph& graph_     (持久化 + 临时数据共存)
        ├── Schema            (label 标记 temporary=true/false)
        ├── VertexTable[]     (持久化 + 临时 vertex 混存)
        └── EdgeTable[]       (持久化 + 临时 edge 混存)

查询时无需合并，直接使用现有 PropertyGraph 接口
```

### 优势

1. **查询路径零改动**：临时 label 和持久化 label 在同一个 label_t 空间和 VID 空间中，现有的遍历、扫描、属性读取全部复用
2. **无需合并层**：不需要实现 UnifiedGraphView，不需要合并两个 PropertyGraph 的扫描结果
3. **无需 VID 映射**：临时边引用持久化点时直接在同一个 PropertyGraph 中解析，不存在跨图映射问题
4. **实现简单**：核心改动集中在 Schema 加标记 + Dump 跳过 + 清理逻辑

### 劣势

1. **Checkpoint 需要小心**：PropertyGraph::Dump() 遍历所有 VertexTable/EdgeTable 时必须正确跳过 temporary label，遗漏则污染磁盘
2. **Connection 隔离需额外机制**：临时数据在共享 PropertyGraph 中，其他 connection 天然可见。如需隔离，需在 connection 上记录其创建的 temp label 列表，查询时过滤
3. **清理逻辑散落**：connection 关闭时需主动遍历 Schema 查找 temporary label，逐个删除数据和 schema entry
4. **数据泄漏风险**：如果某个代码路径忘了过滤 temporary label，可能导致意外的数据可见性
5. **Schema 污染**：临时 label 混在持久化 Schema 中，DumpSchema 也需要跳过
6. **WAL 需要注意**：如果通过事务层操作临时数据，需要区分跳过 WAL；如果绕过事务层直接操作 PropertyGraph，则无此问题

## READ_ONLY 模式下的临时图

READ_ONLY 模式允许多 connection 并发读取持久化图。在 AP 场景中，一个常见需求是：用户以 READ_ONLY 连接到生产图，加载外部数据做关联分析后丢弃。DuckDB 支持此场景（READ_ONLY 数据库仍可创建临时表）。

| | 方案 A（独立存储） | 方案 B（共享存储） |
|--|--|--|
| READ_ONLY + 多 connection | 天然支持，各 connection 有独立临时 PropertyGraph | 临时数据在共享 PropertyGraph 中，多 connection 需要隔离机制 |

**影响**：如果 READ_ONLY 临时图是近期目标，方案 B 需要提前引入 connection 隔离（Schema 标记归属 + 查询时过滤），否则多 connection 的临时数据互相可见。方案 A 在此场景下无额外成本。

**第一版决策**：先只支持 READ_WRITE/WRITE 模式（单 connection），READ_ONLY 临时图作为后续扩展。

## 对比总结

| 维度                                        | 方案 A（独立存储）     | 方案 B（共享存储）              |
| ------------------------------------------- | ---------------------- | ------------------------------- |
| Connection 隔离                             | 天然支持               | 需额外机制                      |
| READ_ONLY 多 connection 临时图              | 天然支持               | 需隔离机制才可用                |
| 查询路径改动                                | 需合并层 + VID 映射    | 无需改动                        |
| WAL 跳过                                    | 天然（DummyWalWriter） | 绕过事务层，或按 label 判断跳过 |
| Checkpoint 安全性                           | 天然安全               | 需按 label 判断跳过             |
| 生命周期管理                                | 销毁实例即可           | 需扫描清理                      |
| 实现复杂度（第一版）                        | 较高（合并层）         | 较低                            |
| 后续扩展成本（如果要支持多图/多connection） | 较低（隔离已具备）     | 较高（需加隔离）                |

## DuckDB 选择独立存储的原因

DuckDB 选择方案 A（独立 AttachedDatabase），主要因为：

1. **已有 AttachedDatabase 抽象**：DuckDB 本身支持 ATTACH 多个数据库，加一个 temp AttachedDatabase 是复用现有架构，边际成本低
2. **Connection 隔离是刚需**：DuckDB 支持多 connection 并发，每个 connection 的临时表必须互不可见
3. **关系型 table scan 合并成本低**：DuckDB 的 table scan 是相对轻量的操作，合并两个 catalog 的查询结果并不复杂
4. **避免 flag 判断散落**：如果把临时数据混在持久化存储中，WAL、checkpoint、catalog 查找、事务管理等每一个路径都需要加 `if (temporary)` 判断，维护成本高且容易遗漏

## NeuG 第一版选择共享存储的原因

1. **避免构建多图基础设施**：方案 A 本质上要求实现多图联合查询能力（合并 Schema、跨图 VID 映射、统一扫描接口）。如果 NeuG 未来不需要通用的多图能力，这部分投入只服务于临时图场景，性价比不高
2. **PropertyGraph 比 DuckDB table 更重**：CSR 结构、VID 索引等使得合并两个 PropertyGraph 的查询结果成本显著高于 DuckDB 的 table scan 合并
3. **第一版不需要 Connection 隔离**：READ_WRITE 模式下只有一个 Connection，隔离天然满足
4. **改动面更集中**：只需改 Schema 标记 + Dump 跳过 + 清理逻辑，不需要动查询路径

## 后续演进路径

如果 Connection 隔离成为刚需，有两条演进路径：

1. **在共享存储上加隔离**：Connection 记录自己创建的 temp label 列表，查询时 GraphEntry 按 connection 过滤
2. **切换到独立存储**：实现 per-connection 临时 PropertyGraph + 查询合并层（方案 A）

路径 1 改动较小但隔离不够彻底（依赖正确过滤），路径 2 更干净但需要实现合并层。

**关于路径 2 的额外考量**：如果 NeuG 未来规划中包含多图能力（如图联邦、跨图查询），那么方案 A 所需的多图基础设施可以服务于更广泛的场景，临时图只是第一个应用。此时方案 A 的投入回报比会显著提升。反之，如果多图不在路线图上，方案 A 的基础设施就是纯粹为临时图服务的专用成本。
