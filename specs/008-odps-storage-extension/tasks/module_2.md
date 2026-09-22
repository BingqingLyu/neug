# Module 2: ODPS → 图表批量导入 (Priority: P2)

**Goal**: 让用户用一条 `COPY <table> FROM (LOAD FROM "odps://..." RETURN ...)` 把 ODPS 表灌入 NeuG 点表/边表，
支持列重映射与 src/dst 主键映射。依赖 Module 1 的扫描能力。对应 FR-007~010，SC-003。

**Assignee**: [TBD — 需确认]
**Label**: [TBD — 建议 `extension` / `odps`]
**Milestone**: [TBD]
**Project**: [TBD]

## [F008-T201] 打通 COPY 点表灌入

**description**: 支持 `COPY <node_table> FROM (LOAD FROM "odps://..." RETURN ...)`，把 ODPS 表作为点表数据源批量灌入（FR-007）。

**details**:
* 复用 NeuG 现有 `COPY ... FROM (LOAD FROM ...)` 融合路径（`supplierFunc` 直供 COPY，避免中间物化）；不新增灌图机制。
* 主键列按目标 `CREATE NODE TABLE` 定义绑定；`MATCH (p:person) RETURN count(p)` 等于源表行数、抽样属性一致（Acceptance 1，SC-003）。
* 依赖 M1 的 read/supply 已稳定。

## [F008-T202] 边表灌入并映射 src/dst 主键列

**description**: 支持把 ODPS 表灌入边表，并把指定列映射为起点/终点主键（FR-008）。

**details**:
* 沿用现有 COPY 边表语法中的 src/dst 主键列指定方式（FROM 节点表 / TO 节点表 + 键列），把 ODPS `RETURN` 出的列对应到 src/dst。
* 灌入后边两端正确连到对应点（Acceptance 2）。
* 空/NULL 键值的处理遵循 NeuG 现有 COPY 规则（若目标不允许则报错定位到行/列）。

## [F008-T203] 列重映射（按名/按位对应）

**description**: 支持 `RETURN` 的列顺序/子集与目标表定义不同，按列名或按位置正确映射，不错位（FR-009，A2）。

**details**:
* 复用现有 `LOAD FROM ... RETURN` + `COPY FROM` 的列绑定/重映射能力（`ScanFileBindData::getProjectColumns`），**不新发明映射 DSL**（区别于 GraphScope JSON 映射）。
* 场景：RETURN 列顺序打乱、只取子集、别名映射 → 目标属性按名/按位对齐（Acceptance 3）。

## [F008-T204] 类型契合校验与不兼容报错

**description**: 当 ODPS 列类型无法安全转换为目标图列类型时，报错并指出出错列与类型，绝不静默写错数据（FR-010）。

**details**:
* 遵循 NeuG 现有 COPY 的强制转换/报错规则；转换失败定位到 `列名 + 源类型 → 目标类型`。
* 与 M1 类型映射矩阵协同（M4 补全复杂类型后再回归此校验）。

## [F008-T205] Module 2 单测 + 集成测

**description**: 覆盖 M2 的映射与灌图正确性（对齐 spec M2 Test Strategy）。

**details**:
* 单测（无网络）：列映射/重映射解析、边表 src/dst 主键列绑定、类型不兼容报错路径。
* 集成测：ODPS→点表、ODPS→边表端到端灌入 + `MATCH` 校验点数/边数与抽样属性；列顺序错乱场景；类型不匹配报错场景。
* 复用 M1 的 Mock/录制回放或 `skipif` 凭据门控。
