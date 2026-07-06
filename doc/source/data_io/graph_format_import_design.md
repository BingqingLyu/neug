# Graph Format Import Design Reference

## Background

NeuG 当前的 `LOAD FROM` 设计面向**单表数据源**（CSV、Parquet、JSON、Iceberg），每次调用返回一个平坦的表结果集。但对于**图格式数据源**（如 GraphAr、Neptune Export 等），一个数据集本身包含多种顶点类型和边类型，具有完整的图结构信息。

本文档梳理从图格式数据源导入数据到 NeuG 的可选方案。

---

## 当前能力

| 操作 | 语义 | 持久化 | 返回类型 |
|------|------|--------|----------|
| `LOAD FROM <path> RETURN *` | 读取单张表，返回临时结果集 | 否 | 平坦表（行+列） |
| `COPY <Table> FROM <path>` | 将数据写入一张 Node/Rel Table | 是 | 无 |

**限制**：`LOAD FROM` 无法在一次调用中返回"多张表"或"一整张图"的结构。

---

## 方案 A：多次 LOAD FROM + COPY FROM（当前可行）

### 思路

将图格式扩展（如 GraphAr）注册为 `ReadFunction`，通过 `table` 参数指定读取哪种实体。用户逐个导入每种顶点和边。

### 语法示例

```cypher
-- 1. 加载扩展
LOAD graphar;

-- 2. 预览顶点数据
LOAD FROM "graphar:///path/to/graph" (table='person', FILE_FORMAT='graphar')
RETURN *

-- 3. 导入顶点
COPY person FROM (
    LOAD FROM "graphar:///path/to/graph" (table='person', FILE_FORMAT='graphar')
    RETURN id, name, age
);

-- 4. 导入边
COPY knows FROM (
    LOAD FROM "graphar:///path/to/graph" (table='knows', FILE_FORMAT='graphar')
    RETURN src_id, dst_id, weight
) (from="person", to="person");
```

### 优缺点

| 优点 | 缺点 |
|------|------|
| 无需修改核心语法 | 用户需要手动枚举每种实体 |
| 完全复用现有 LOAD FROM + COPY FROM 管线 | 对于包含 10+ 种实体的图，操作繁琐 |
| 支持列映射、过滤、类型转换 | 无法一键导入整张图 |
| 扩展开发成本低 | — |

### 适用场景

- 图格式简单（1-3 种实体类型）
- 需要精细控制列映射和类型转换
- 增量导入某种特定实体

---

## 方案 B：COPY GRAPH FROM（新语法）

### 思路

新增 `COPY GRAPH FROM` 语句，一键将整张图导入 NeuG。内部自动完成：
1. 解析图数据源的 schema（所有顶点类型、边类型及属性）
2. 自动创建对应的 Node Table 和 Rel Table
3. 批量导入所有数据

### 语法示例

```cypher
-- 一键导入整张图
COPY GRAPH FROM "graphar:///path/to/graph";

-- 指定参数
COPY GRAPH FROM "oss://bucket/graph_data" (
    FILE_FORMAT='graphar',
    CREDENTIALS_KIND='Default',
    ENDPOINT_OVERRIDE='oss-cn-beijing.aliyuncs.com'
);

-- 导入后可直接查询
MATCH (p:person)-[k:knows]->(f:person)
RETURN p.name, f.name, k.weight;
```

### 内部执行流程

```
COPY GRAPH FROM <path>
  → 图格式扩展解析整体 schema（顶点类型列表、边类型列表、各自属性和主键）
  → 为每种顶点类型执行 CREATE NODE TABLE（如不存在）
  → 为每种边类型执行 CREATE REL TABLE（如不存在）
  → 按拓扑顺序批量导入：先顶点，后边
  → 返回导入统计信息
```

### 优缺点

| 优点 | 缺点 |
|------|------|
| 一键体验，用户无需了解图内部结构 | 需要新增语法解析（ANTLR grammar） |
| 自动处理表创建和拓扑依赖 | 需要新的执行算子 |
| 适合大规模图数据迁移 | 灵活性较低（无法对单表做精细映射） |
| 可结合 auto_detect 特性 | — |

### 适用场景

- 整图迁移 / 备份恢复
- 图格式包含多种实体类型
- 用户不需要精细控制，只需"一键加载"

---

## 方案 C：扩展命令 IMPORT GRAPH（扩展层实现）

### 思路

不修改核心语法，而是让图格式扩展注册一个**自定义函数/命令**（如 `CALL import_graph(...)`），在扩展内部完成整图导入逻辑。

### 语法示例

```cypher
-- 通过 CALL 调用扩展提供的导入函数
CALL import_graph('graphar:///path/to/graph');

-- 或通过 table function 风格
CALL graphar.import(
    path := 'oss://bucket/graph_data',
    credentials_kind := 'Default',
    endpoint_override := 'oss-cn-beijing.aliyuncs.com'
);
```

### 优缺点

| 优点 | 缺点 |
|------|------|
| 不修改核心 parser/binder | 需要 NeuG 支持 CALL/procedure 语法 |
| 扩展间独立，各图格式自行实现 | 语义不如 COPY GRAPH FROM 自然 |
| 部署灵活，按需安装 | 可能缺少事务保证 |

### 适用场景

- NeuG 已支持存储过程/CALL 语法
- 希望各图格式扩展完全解耦
- 快速原型验证

---

## 方案对比总结

| 维度 | 方案 A | 方案 B | 方案 C |
|------|--------|--------|--------|
| 用户体验 | 手动逐表 | 一键导入 | 一键导入 |
| 核心改动 | 无 | 新语法 + 新算子 | 需 CALL 支持 |
| 扩展开发量 | 低（注册 ReadFunction） | 中（注册 GraphImporter） | 中（注册 Procedure） |
| 灵活性 | 高（逐表精细控制） | 中（整图粒度） | 中 |
| 可组合性 | 可与 WHERE/CAST 组合 | 整图级别，难组合 | 取决于实现 |
| 可用时间 | 现在 | 需开发 | 需开发 |

---

## 建议路线

1. **短期**：用方案 A 支持 GraphAr 等图格式的单表读取（复用 LOAD FROM 框架）
2. **中期**：实现方案 B `COPY GRAPH FROM`，提供一站式整图导入体验
3. **可选**：`LOAD FROM` 增加 `RETURN GRAPH SCHEMA` 能力，用于预览图结构

### LOAD FROM 在图格式中的定位

| 层次 | 操作 | 用途 |
|------|------|------|
| 表级别探索 | `LOAD FROM ... (table='person') RETURN *` | 预览某种实体的数据 |
| 图 schema 探索 | `LOAD FROM ... RETURN GRAPH SCHEMA`（待设计） | 查看整图结构 |
| 单表持久化导入 | `COPY <T> FROM (LOAD FROM ...)` | 导入一种实体 |
| 整图持久化导入 | `COPY GRAPH FROM ...`（待实现） | 一键导入全部实体 |
